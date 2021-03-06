// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/incremental-marking.h"

#include "src/code-stubs.h"
#include "src/compilation-cache.h"
#include "src/conversions.h"
#include "src/heap/concurrent-marking.h"
#include "src/heap/gc-idle-time-handler.h"
#include "src/heap/gc-tracer.h"
#include "src/heap/heap-inl.h"
#include "src/heap/mark-compact-inl.h"
#include "src/heap/object-stats.h"
#include "src/heap/objects-visiting-inl.h"
#include "src/heap/objects-visiting.h"
#include "src/tracing/trace-event.h"
#include "src/v8.h"
#include "src/visitors.h"
#include "src/vm-state-inl.h"

namespace v8 {
namespace internal {

void IncrementalMarking::Observer::Step(int bytes_allocated, Address, size_t) {
  VMState<GC> state(incremental_marking_.heap()->isolate());
  RuntimeCallTimerScope runtime_timer(
      incremental_marking_.heap()->isolate(),
      &RuntimeCallStats::GC_Custom_IncrementalMarkingObserver);
  incremental_marking_.AdvanceIncrementalMarkingOnAllocation();
}

IncrementalMarking::IncrementalMarking(Heap* heap)
    : heap_(heap),
      marking_worklist_(nullptr),
      initial_old_generation_size_(0),
      bytes_marked_ahead_of_schedule_(0),
      unscanned_bytes_of_large_object_(0),
      idle_marking_delay_counter_(0),
      incremental_marking_finalization_rounds_(0),
      is_compacting_(false),
      should_hurry_(false),
      was_activated_(false),
      black_allocation_(false),
      finalize_marking_completed_(false),
      trace_wrappers_toggle_(false),
      request_type_(NONE),
      new_generation_observer_(*this, kAllocatedThreshold),
      old_generation_observer_(*this, kAllocatedThreshold) {
  SetState(STOPPED);
}

bool IncrementalMarking::BaseRecordWrite(HeapObject* obj, Object* value) {
  HeapObject* value_heap_obj = HeapObject::cast(value);
  DCHECK(!ObjectMarking::IsImpossible<kAtomicity>(
      value_heap_obj, marking_state(value_heap_obj)));
  DCHECK(!ObjectMarking::IsImpossible<kAtomicity>(obj, marking_state(obj)));
  const bool need_recording =
      FLAG_concurrent_marking ||
      ObjectMarking::IsBlack<kAtomicity>(obj, marking_state(obj));

  if (need_recording && WhiteToGreyAndPush(value_heap_obj)) {
    RestartIfNotMarking();
  }
  return is_compacting_ && need_recording;
}


void IncrementalMarking::RecordWriteSlow(HeapObject* obj, Object** slot,
                                         Object* value) {
  if (BaseRecordWrite(obj, value) && slot != NULL) {
    // Object is not going to be rescanned we need to record the slot.
    heap_->mark_compact_collector()->RecordSlot(obj, slot, value);
  }
}

int IncrementalMarking::RecordWriteFromCode(HeapObject* obj, Object** slot,
                                            Isolate* isolate) {
  DCHECK(obj->IsHeapObject());
  isolate->heap()->incremental_marking()->RecordWrite(obj, slot, *slot);
  // Called by RecordWriteCodeStubAssembler, which doesnt accept void type
  return 0;
}

void IncrementalMarking::RecordCodeTargetPatch(Code* host, Address pc,
                                               HeapObject* value) {
  if (IsMarking()) {
    RelocInfo rinfo(pc, RelocInfo::CODE_TARGET, 0, host);
    RecordWriteIntoCode(host, &rinfo, value);
  }
}


void IncrementalMarking::RecordCodeTargetPatch(Address pc, HeapObject* value) {
  if (IsMarking()) {
    Code* host = heap_->isolate()
                     ->inner_pointer_to_code_cache()
                     ->GcSafeFindCodeForInnerPointer(pc);
    RelocInfo rinfo(pc, RelocInfo::CODE_TARGET, 0, host);
    RecordWriteIntoCode(host, &rinfo, value);
  }
}


void IncrementalMarking::RecordWriteIntoCodeSlow(Code* host, RelocInfo* rinfo,
                                                 Object* value) {
  if (BaseRecordWrite(host, value)) {
    // Object is not going to be rescanned.  We need to record the slot.
    heap_->mark_compact_collector()->RecordRelocSlot(host, rinfo, value);
  }
}

bool IncrementalMarking::WhiteToGreyAndPush(HeapObject* obj) {
  if (ObjectMarking::WhiteToGrey<kAtomicity>(obj, marking_state(obj))) {
    marking_worklist()->Push(obj);
    return true;
  }
  return false;
}

void IncrementalMarking::MarkBlackAndPush(HeapObject* obj) {
  // Color the object black and push it into the bailout deque.
  ObjectMarking::WhiteToGrey<kAtomicity>(obj, marking_state(obj));
  if (ObjectMarking::GreyToBlack<kAtomicity>(obj, marking_state(obj))) {
#ifdef V8_CONCURRENT_MARKING
    marking_worklist()->PushBailout(obj);
#else
    if (!marking_worklist()->Push(obj)) {
      ObjectMarking::BlackToGrey<kAtomicity>(obj, marking_state(obj));
    }
#endif
  }
}

void IncrementalMarking::NotifyLeftTrimming(HeapObject* from, HeapObject* to) {
  DCHECK(IsMarking());
  DCHECK(MemoryChunk::FromAddress(from->address())->SweepingDone());
  DCHECK_EQ(MemoryChunk::FromAddress(from->address()),
            MemoryChunk::FromAddress(to->address()));
  DCHECK_NE(from, to);

  MarkBit old_mark_bit = ObjectMarking::MarkBitFrom(from, marking_state(from));
  MarkBit new_mark_bit = ObjectMarking::MarkBitFrom(to, marking_state(to));

  if (black_allocation() && Marking::IsBlack<kAtomicity>(new_mark_bit)) {
    // Nothing to do if the object is in black area.
    return;
  }

  bool marked_black_due_to_left_trimming = false;
  if (FLAG_concurrent_marking) {
    // We need to mark the array black before overwriting its map and length
    // so that the concurrent marker does not observe inconsistent state.
    Marking::WhiteToGrey<kAtomicity>(old_mark_bit);
    if (Marking::GreyToBlack<kAtomicity>(old_mark_bit)) {
      // The concurrent marker will not mark the array. We need to push the
      // new array start in marking deque to ensure that it will be marked.
      marked_black_due_to_left_trimming = true;
    }
    DCHECK(Marking::IsBlack<kAtomicity>(old_mark_bit));
  }

  if (Marking::IsBlack<kAtomicity>(old_mark_bit) &&
      !marked_black_due_to_left_trimming) {
    // The array was black before left trimming or was marked black by the
    // concurrent marker. Simply transfer the color.
    if (from->address() + kPointerSize == to->address()) {
      // The old and the new markbits overlap. The |to| object has the
      // grey color. To make it black, we need to set the second bit.
      DCHECK(new_mark_bit.Get<kAtomicity>());
      new_mark_bit.Next().Set<kAtomicity>();
    } else {
      bool success = Marking::WhiteToBlack<kAtomicity>(new_mark_bit);
      DCHECK(success);
      USE(success);
    }
  } else if (Marking::IsGrey<kAtomicity>(old_mark_bit) ||
             marked_black_due_to_left_trimming) {
    // The array was already grey or was marked black by this function.
    // Mark the new array grey and push it to marking deque.
    if (from->address() + kPointerSize == to->address()) {
      // The old and the new markbits overlap. The |to| object is either white
      // or grey.  Set the first bit to make sure that it is grey.
      new_mark_bit.Set();
      DCHECK(!new_mark_bit.Next().Get());
    } else {
      bool success = Marking::WhiteToGrey<kAtomicity>(new_mark_bit);
      DCHECK(success);
      USE(success);
    }
    marking_worklist()->Push(to);
    RestartIfNotMarking();
  }
}

class IncrementalMarkingMarkingVisitor final
    : public MarkingVisitor<IncrementalMarkingMarkingVisitor> {
 public:
  typedef MarkingVisitor<IncrementalMarkingMarkingVisitor> Parent;

  static const int kProgressBarScanningChunk = 32 * 1024;

  explicit IncrementalMarkingMarkingVisitor(MarkCompactCollector* collector)
      : MarkingVisitor<IncrementalMarkingMarkingVisitor>(collector->heap(),
                                                         collector),
        incremental_marking_(collector->heap()->incremental_marking()) {}

  V8_INLINE int VisitFixedArray(Map* map, FixedArray* object) {
    MemoryChunk* chunk = MemoryChunk::FromAddress(object->address());
    int object_size = FixedArray::BodyDescriptor::SizeOf(map, object);
    if (chunk->IsFlagSet(MemoryChunk::HAS_PROGRESS_BAR)) {
      DCHECK(!FLAG_use_marking_progress_bar ||
             chunk->owner()->identity() == LO_SPACE);
      // When using a progress bar for large fixed arrays, scan only a chunk of
      // the array and try to push it onto the marking deque again until it is
      // fully scanned. Fall back to scanning it through to the end in case this
      // fails because of a full deque.
      int start_offset =
          Max(FixedArray::BodyDescriptor::kStartOffset, chunk->progress_bar());
      if (start_offset < object_size) {
#ifdef V8_CONCURRENT_MARKING
        incremental_marking_->marking_worklist()->PushBailout(object);
#else
        if (ObjectMarking::IsGrey<IncrementalMarking::kAtomicity>(
                object, incremental_marking_->marking_state(object))) {
          incremental_marking_->marking_worklist()->Push(object);
        } else {
          DCHECK(ObjectMarking::IsBlack<IncrementalMarking::kAtomicity>(
              object, incremental_marking_->marking_state(object)));
          collector_->PushBlack(object);
        }
#endif
        int end_offset =
            Min(object_size, start_offset + kProgressBarScanningChunk);
        int already_scanned_offset = start_offset;
        bool scan_until_end = false;
        do {
          VisitPointers(object, HeapObject::RawField(object, start_offset),
                        HeapObject::RawField(object, end_offset));
          start_offset = end_offset;
          end_offset = Min(object_size, end_offset + kProgressBarScanningChunk);
          scan_until_end = incremental_marking_->marking_worklist()->IsFull();
        } while (scan_until_end && start_offset < object_size);
        chunk->set_progress_bar(start_offset);
        if (start_offset < object_size) {
          incremental_marking_->NotifyIncompleteScanOfObject(
              object_size - (start_offset - already_scanned_offset));
        }
      }
    } else {
      FixedArray::BodyDescriptor::IterateBody(object, object_size, this);
    }
    return object_size;
  }

  V8_INLINE int VisitNativeContext(Map* map, Context* context) {
    // We will mark cache black with a separate pass when we finish marking.
    // Note that GC can happen when the context is not fully initialized,
    // so the cache can be undefined.
    Object* cache = context->get(Context::NORMALIZED_MAP_CACHE_INDEX);
    if (!cache->IsUndefined(map->GetIsolate())) {
      if (cache->IsHeapObject()) {
        HeapObject* heap_obj = HeapObject::cast(cache);
        // Mark the object grey if it is white, do not enque it into the marking
        // deque.
        ObjectMarking::WhiteToGrey<IncrementalMarking::kAtomicity>(
            heap_obj, incremental_marking_->marking_state(heap_obj));
      }
    }
    return Parent::VisitNativeContext(map, context);
  }

  V8_INLINE void VisitPointer(HeapObject* host, Object** p) final {
    Object* target = *p;
    if (target->IsHeapObject()) {
      collector_->RecordSlot(host, p, target);
      MarkObject(host, target);
    }
  }

  V8_INLINE void VisitPointers(HeapObject* host, Object** start,
                               Object** end) final {
    for (Object** p = start; p < end; p++) {
      Object* target = *p;
      if (target->IsHeapObject()) {
        collector_->RecordSlot(host, p, target);
        MarkObject(host, target);
      }
    }
  }

  // Marks the object grey and pushes it on the marking stack.
  V8_INLINE void MarkObject(HeapObject* host, Object* obj) {
    incremental_marking_->WhiteToGreyAndPush(HeapObject::cast(obj));
  }

  // Marks the object black without pushing it on the marking stack.
  // Returns true if object needed marking and false otherwise.
  V8_INLINE bool MarkObjectWithoutPush(HeapObject* host, Object* obj) {
    HeapObject* heap_object = HeapObject::cast(obj);
    return ObjectMarking::WhiteToBlack<IncrementalMarking::kAtomicity>(
        heap_object, incremental_marking_->marking_state(heap_object));
  }

 private:
  IncrementalMarking* const incremental_marking_;
};

class IncrementalMarkingRootMarkingVisitor : public RootVisitor {
 public:
  explicit IncrementalMarkingRootMarkingVisitor(
      IncrementalMarking* incremental_marking)
      : heap_(incremental_marking->heap()) {}

  void VisitRootPointer(Root root, Object** p) override {
    MarkObjectByPointer(p);
  }

  void VisitRootPointers(Root root, Object** start, Object** end) override {
    for (Object** p = start; p < end; p++) MarkObjectByPointer(p);
  }

 private:
  void MarkObjectByPointer(Object** p) {
    Object* obj = *p;
    if (!obj->IsHeapObject()) return;

    heap_->incremental_marking()->WhiteToGreyAndPush(HeapObject::cast(obj));
  }

  Heap* heap_;
};

void IncrementalMarking::SetOldSpacePageFlags(MemoryChunk* chunk,
                                              bool is_marking,
                                              bool is_compacting) {
  if (is_marking) {
    chunk->SetFlag(MemoryChunk::POINTERS_TO_HERE_ARE_INTERESTING);
    chunk->SetFlag(MemoryChunk::POINTERS_FROM_HERE_ARE_INTERESTING);
  } else {
    chunk->ClearFlag(MemoryChunk::POINTERS_TO_HERE_ARE_INTERESTING);
    chunk->SetFlag(MemoryChunk::POINTERS_FROM_HERE_ARE_INTERESTING);
  }
}


void IncrementalMarking::SetNewSpacePageFlags(MemoryChunk* chunk,
                                              bool is_marking) {
  chunk->SetFlag(MemoryChunk::POINTERS_TO_HERE_ARE_INTERESTING);
  if (is_marking) {
    chunk->SetFlag(MemoryChunk::POINTERS_FROM_HERE_ARE_INTERESTING);
  } else {
    chunk->ClearFlag(MemoryChunk::POINTERS_FROM_HERE_ARE_INTERESTING);
  }
}


void IncrementalMarking::DeactivateIncrementalWriteBarrierForSpace(
    PagedSpace* space) {
  for (Page* p : *space) {
    SetOldSpacePageFlags(p, false, false);
  }
}


void IncrementalMarking::DeactivateIncrementalWriteBarrierForSpace(
    NewSpace* space) {
  for (Page* p : *space) {
    SetNewSpacePageFlags(p, false);
  }
}


void IncrementalMarking::DeactivateIncrementalWriteBarrier() {
  DeactivateIncrementalWriteBarrierForSpace(heap_->old_space());
  DeactivateIncrementalWriteBarrierForSpace(heap_->map_space());
  DeactivateIncrementalWriteBarrierForSpace(heap_->code_space());
  DeactivateIncrementalWriteBarrierForSpace(heap_->new_space());

  for (LargePage* lop : *heap_->lo_space()) {
    SetOldSpacePageFlags(lop, false, false);
  }
}


void IncrementalMarking::ActivateIncrementalWriteBarrier(PagedSpace* space) {
  for (Page* p : *space) {
    SetOldSpacePageFlags(p, true, is_compacting_);
  }
}


void IncrementalMarking::ActivateIncrementalWriteBarrier(NewSpace* space) {
  for (Page* p : *space) {
    SetNewSpacePageFlags(p, true);
  }
}


void IncrementalMarking::ActivateIncrementalWriteBarrier() {
  ActivateIncrementalWriteBarrier(heap_->old_space());
  ActivateIncrementalWriteBarrier(heap_->map_space());
  ActivateIncrementalWriteBarrier(heap_->code_space());
  ActivateIncrementalWriteBarrier(heap_->new_space());

  for (LargePage* lop : *heap_->lo_space()) {
    SetOldSpacePageFlags(lop, true, is_compacting_);
  }
}


bool IncrementalMarking::WasActivated() { return was_activated_; }


bool IncrementalMarking::CanBeActivated() {
  // Only start incremental marking in a safe state: 1) when incremental
  // marking is turned on, 2) when we are currently not in a GC, and
  // 3) when we are currently not serializing or deserializing the heap.
  return FLAG_incremental_marking && heap_->gc_state() == Heap::NOT_IN_GC &&
         heap_->deserialization_complete() &&
         !heap_->isolate()->serializer_enabled();
}


void IncrementalMarking::ActivateGeneratedStub(Code* stub) {
  DCHECK(RecordWriteStub::GetMode(stub) == RecordWriteStub::STORE_BUFFER_ONLY);

  if (!IsMarking()) {
    // Initially stub is generated in STORE_BUFFER_ONLY mode thus
    // we don't need to do anything if incremental marking is
    // not active.
  } else if (IsCompacting()) {
    RecordWriteStub::Patch(stub, RecordWriteStub::INCREMENTAL_COMPACTION);
  } else {
    RecordWriteStub::Patch(stub, RecordWriteStub::INCREMENTAL);
  }
}


static void PatchIncrementalMarkingRecordWriteStubs(
    Heap* heap, RecordWriteStub::Mode mode) {
  UnseededNumberDictionary* stubs = heap->code_stubs();

  int capacity = stubs->Capacity();
  Isolate* isolate = heap->isolate();
  for (int i = 0; i < capacity; i++) {
    Object* k = stubs->KeyAt(i);
    if (stubs->IsKey(isolate, k)) {
      uint32_t key = NumberToUint32(k);

      if (CodeStub::MajorKeyFromKey(key) == CodeStub::RecordWrite) {
        Object* e = stubs->ValueAt(i);
        if (e->IsCode()) {
          RecordWriteStub::Patch(Code::cast(e), mode);
        }
      }
    }
  }
}

void IncrementalMarking::Start(GarbageCollectionReason gc_reason) {
  if (FLAG_trace_incremental_marking) {
    int old_generation_size_mb =
        static_cast<int>(heap()->PromotedSpaceSizeOfObjects() / MB);
    int old_generation_limit_mb =
        static_cast<int>(heap()->old_generation_allocation_limit() / MB);
    heap()->isolate()->PrintWithTimestamp(
        "[IncrementalMarking] Start (%s): old generation %dMB, limit %dMB, "
        "slack %dMB\n",
        Heap::GarbageCollectionReasonToString(gc_reason),
        old_generation_size_mb, old_generation_limit_mb,
        Max(0, old_generation_limit_mb - old_generation_size_mb));
  }
  DCHECK(FLAG_incremental_marking);
  DCHECK(state_ == STOPPED);
  DCHECK(heap_->gc_state() == Heap::NOT_IN_GC);
  DCHECK(!heap_->isolate()->serializer_enabled());

  Counters* counters = heap_->isolate()->counters();

  counters->incremental_marking_reason()->AddSample(
      static_cast<int>(gc_reason));
  HistogramTimerScope incremental_marking_scope(
      counters->gc_incremental_marking_start());
  TRACE_EVENT0("v8", "V8.GCIncrementalMarkingStart");
  heap_->tracer()->NotifyIncrementalMarkingStart();

  start_time_ms_ = heap()->MonotonicallyIncreasingTimeInMs();
  initial_old_generation_size_ = heap_->PromotedSpaceSizeOfObjects();
  old_generation_allocation_counter_ = heap_->OldGenerationAllocationCounter();
  bytes_allocated_ = 0;
  bytes_marked_ahead_of_schedule_ = 0;
  should_hurry_ = false;
  was_activated_ = true;

  if (!heap_->mark_compact_collector()->sweeping_in_progress()) {
    StartMarking();
  } else {
    if (FLAG_trace_incremental_marking) {
      heap()->isolate()->PrintWithTimestamp(
          "[IncrementalMarking] Start sweeping.\n");
    }
    SetState(SWEEPING);
  }

  SpaceIterator it(heap_);
  while (it.has_next()) {
    Space* space = it.next();
    if (space == heap_->new_space()) {
      space->AddAllocationObserver(&new_generation_observer_);
    } else {
      space->AddAllocationObserver(&old_generation_observer_);
    }
  }

  incremental_marking_job()->Start(heap_);
}


void IncrementalMarking::StartMarking() {
  if (heap_->isolate()->serializer_enabled()) {
    // Black allocation currently starts when we start incremental marking,
    // but we cannot enable black allocation while deserializing. Hence, we
    // have to delay the start of incremental marking in that case.
    if (FLAG_trace_incremental_marking) {
      heap()->isolate()->PrintWithTimestamp(
          "[IncrementalMarking] Start delayed - serializer\n");
    }
    return;
  }
  if (FLAG_trace_incremental_marking) {
    heap()->isolate()->PrintWithTimestamp(
        "[IncrementalMarking] Start marking\n");
  }

  is_compacting_ =
      !FLAG_never_compact && heap_->mark_compact_collector()->StartCompaction();

  SetState(MARKING);

  {
    TRACE_GC(heap()->tracer(),
             GCTracer::Scope::MC_INCREMENTAL_WRAPPER_PROLOGUE);
    heap_->local_embedder_heap_tracer()->TracePrologue();
  }

  RecordWriteStub::Mode mode = is_compacting_
                                   ? RecordWriteStub::INCREMENTAL_COMPACTION
                                   : RecordWriteStub::INCREMENTAL;

  PatchIncrementalMarkingRecordWriteStubs(heap_, mode);

  marking_worklist()->StartUsing();

  ActivateIncrementalWriteBarrier();

// Marking bits are cleared by the sweeper.
#ifdef VERIFY_HEAP
  if (FLAG_verify_heap) {
    heap_->mark_compact_collector()->VerifyMarkbitsAreClean();
  }
#endif

  heap_->isolate()->compilation_cache()->MarkCompactPrologue();

  if (FLAG_concurrent_marking && !black_allocation_) {
    StartBlackAllocation();
  }

  // Mark strong roots grey.
  IncrementalMarkingRootMarkingVisitor visitor(this);
  heap_->IterateStrongRoots(&visitor, VISIT_ONLY_STRONG);

  if (FLAG_concurrent_marking) {
    heap_->concurrent_marking()->ScheduleTasks();
  }

  // Ready to start incremental marking.
  if (FLAG_trace_incremental_marking) {
    heap()->isolate()->PrintWithTimestamp("[IncrementalMarking] Running\n");
  }
}

void IncrementalMarking::StartBlackAllocation() {
  DCHECK(FLAG_black_allocation);
  DCHECK(IsMarking());
  black_allocation_ = true;
  heap()->old_space()->MarkAllocationInfoBlack();
  heap()->map_space()->MarkAllocationInfoBlack();
  heap()->code_space()->MarkAllocationInfoBlack();
  if (FLAG_trace_incremental_marking) {
    heap()->isolate()->PrintWithTimestamp(
        "[IncrementalMarking] Black allocation started\n");
  }
}

void IncrementalMarking::PauseBlackAllocation() {
  DCHECK(FLAG_black_allocation);
  DCHECK(IsMarking());
  heap()->old_space()->UnmarkAllocationInfo();
  heap()->map_space()->UnmarkAllocationInfo();
  heap()->code_space()->UnmarkAllocationInfo();
  if (FLAG_trace_incremental_marking) {
    heap()->isolate()->PrintWithTimestamp(
        "[IncrementalMarking] Black allocation paused\n");
  }
  black_allocation_ = false;
}

void IncrementalMarking::FinishBlackAllocation() {
  if (black_allocation_) {
    black_allocation_ = false;
    if (FLAG_trace_incremental_marking) {
      heap()->isolate()->PrintWithTimestamp(
          "[IncrementalMarking] Black allocation finished\n");
    }
  }
}

void IncrementalMarking::AbortBlackAllocation() {
  if (FLAG_trace_incremental_marking) {
    heap()->isolate()->PrintWithTimestamp(
        "[IncrementalMarking] Black allocation aborted\n");
  }
}

void IncrementalMarking::MarkRoots() {
  DCHECK(!finalize_marking_completed_);
  DCHECK(IsMarking());

  IncrementalMarkingRootMarkingVisitor visitor(this);
  heap_->IterateStrongRoots(&visitor, VISIT_ONLY_STRONG);
}

bool ShouldRetainMap(Map* map, int age) {
  if (age == 0) {
    // The map has aged. Do not retain this map.
    return false;
  }
  Object* constructor = map->GetConstructor();
  Heap* heap = map->GetHeap();
  if (!constructor->IsHeapObject() ||
      ObjectMarking::IsWhite<IncrementalMarking::kAtomicity>(
          HeapObject::cast(constructor),
          heap->incremental_marking()->marking_state(
              HeapObject::cast(constructor)))) {
    // The constructor is dead, no new objects with this map can
    // be created. Do not retain this map.
    return false;
  }
  return true;
}


void IncrementalMarking::RetainMaps() {
  // Do not retain dead maps if flag disables it or there is
  // - memory pressure (reduce_memory_footprint_),
  // - GC is requested by tests or dev-tools (abort_incremental_marking_).
  bool map_retaining_is_disabled = heap()->ShouldReduceMemory() ||
                                   heap()->ShouldAbortIncrementalMarking() ||
                                   FLAG_retain_maps_for_n_gc == 0;
  ArrayList* retained_maps = heap()->retained_maps();
  int length = retained_maps->Length();
  // The number_of_disposed_maps separates maps in the retained_maps
  // array that were created before and after context disposal.
  // We do not age and retain disposed maps to avoid memory leaks.
  int number_of_disposed_maps = heap()->number_of_disposed_maps_;
  for (int i = 0; i < length; i += 2) {
    DCHECK(retained_maps->Get(i)->IsWeakCell());
    WeakCell* cell = WeakCell::cast(retained_maps->Get(i));
    if (cell->cleared()) continue;
    int age = Smi::ToInt(retained_maps->Get(i + 1));
    int new_age;
    Map* map = Map::cast(cell->value());
    if (i >= number_of_disposed_maps && !map_retaining_is_disabled &&
        ObjectMarking::IsWhite<kAtomicity>(map, marking_state(map))) {
      if (ShouldRetainMap(map, age)) {
        WhiteToGreyAndPush(map);
      }
      Object* prototype = map->prototype();
      if (age > 0 && prototype->IsHeapObject() &&
          ObjectMarking::IsWhite<kAtomicity>(
              HeapObject::cast(prototype),
              marking_state(HeapObject::cast(prototype)))) {
        // The prototype is not marked, age the map.
        new_age = age - 1;
      } else {
        // The prototype and the constructor are marked, this map keeps only
        // transition tree alive, not JSObjects. Do not age the map.
        new_age = age;
      }
    } else {
      new_age = FLAG_retain_maps_for_n_gc;
    }
    // Compact the array and update the age.
    if (new_age != age) {
      retained_maps->Set(i + 1, Smi::FromInt(new_age));
    }
  }
}

void IncrementalMarking::FinalizeIncrementally() {
  TRACE_GC(heap()->tracer(), GCTracer::Scope::MC_INCREMENTAL_FINALIZE_BODY);
  DCHECK(!finalize_marking_completed_);
  DCHECK(IsMarking());

  double start = heap_->MonotonicallyIncreasingTimeInMs();

  // After finishing incremental marking, we try to discover all unmarked
  // objects to reduce the marking load in the final pause.
  // 1) We scan and mark the roots again to find all changes to the root set.
  // 2) Age and retain maps embedded in optimized code.
  // 3) Remove weak cell with live values from the list of weak cells, they
  // do not need processing during GC.
  MarkRoots();

  if (incremental_marking_finalization_rounds_ == 0) {
    // Map retaining is needed for perfromance, not correctness,
    // so we can do it only once at the beginning of the finalization.
    RetainMaps();
  }

  int marking_progress =
      heap_->mark_compact_collector()->marking_worklist()->Size() +
      static_cast<int>(
          heap_->local_embedder_heap_tracer()->NumberOfCachedWrappersToTrace());

  double end = heap_->MonotonicallyIncreasingTimeInMs();
  double delta = end - start;
  if (FLAG_trace_incremental_marking) {
    heap()->isolate()->PrintWithTimestamp(
        "[IncrementalMarking] Finalize incrementally round %d, "
        "spent %d ms, marking progress %d.\n",
        static_cast<int>(delta), incremental_marking_finalization_rounds_,
        marking_progress);
  }

  ++incremental_marking_finalization_rounds_;
  if ((incremental_marking_finalization_rounds_ >=
       FLAG_max_incremental_marking_finalization_rounds) ||
      (marking_progress <
       FLAG_min_progress_during_incremental_marking_finalization)) {
    finalize_marking_completed_ = true;
  }

  if (FLAG_black_allocation && !heap()->ShouldReduceMemory() &&
      !black_allocation_) {
    // TODO(hpayer): Move to an earlier point as soon as we make faster marking
    // progress.
    StartBlackAllocation();
  }
}

void IncrementalMarking::UpdateMarkingWorklistAfterScavenge() {
  if (!IsMarking()) return;

  Map* filler_map = heap_->one_pointer_filler_map();

  marking_worklist()->Update([this, filler_map](HeapObject* obj,
                                                HeapObject** out) -> bool {
    DCHECK(obj->IsHeapObject());
    // Only pointers to from space have to be updated.
    if (heap_->InFromSpace(obj)) {
      MapWord map_word = obj->map_word();
      if (!map_word.IsForwardingAddress()) {
        // There may be objects on the marking deque that do not exist anymore,
        // e.g. left trimmed objects or objects from the root set (frames).
        // If these object are dead at scavenging time, their marking deque
        // entries will not point to forwarding addresses. Hence, we can discard
        // them.
        return false;
      }
      HeapObject* dest = map_word.ToForwardingAddress();
      DCHECK_IMPLIES(
          ObjectMarking::IsWhite<kAtomicity>(obj, marking_state(obj)),
          obj->IsFiller());
      *out = dest;
      return true;
    } else if (heap_->InToSpace(obj)) {
      // The object may be on a page that was moved in new space.
      DCHECK(
          Page::FromAddress(obj->address())->IsFlagSet(Page::SWEEP_TO_ITERATE));
      if (ObjectMarking::IsGrey<kAtomicity>(obj, MarkingState::External(obj))) {
        *out = obj;
        return true;
      }
      return false;
    } else {
      // The object may be on a page that was moved from new to old space.
      if (Page::FromAddress(obj->address())
              ->IsFlagSet(Page::SWEEP_TO_ITERATE)) {
        if (ObjectMarking::IsGrey<kAtomicity>(obj,
                                              MarkingState::External(obj))) {
          *out = obj;
          return true;
        }
        return false;
      }
      DCHECK_IMPLIES(
          ObjectMarking::IsWhite<kAtomicity>(obj, marking_state(obj)),
          obj->IsFiller());
      // Skip one word filler objects that appear on the
      // stack when we perform in place array shift.
      if (obj->map() != filler_map) {
        *out = obj;
        return true;
      }
      return false;
    }
  });
}

bool IncrementalMarking::IsFixedArrayWithProgressBar(HeapObject* obj) {
  if (!obj->IsFixedArray()) return false;
  MemoryChunk* chunk = MemoryChunk::FromAddress(obj->address());
  return chunk->IsFlagSet(MemoryChunk::HAS_PROGRESS_BAR);
}

void IncrementalMarking::VisitObject(Map* map, HeapObject* obj, int size) {
  MarkBit mark_bit = ObjectMarking::MarkBitFrom(obj, marking_state(obj));
  DCHECK(Marking::IsGrey<kAtomicity>(mark_bit) ||
         Marking::IsBlack<kAtomicity>(mark_bit));
  USE(mark_bit);
  // The object can already be black in two cases:
  // 1. The object is a fixed array with the progress bar.
  // 2. The object is a JSObject that was colored black before
  //    unsafe layout change.
  // 3. The object is a string that was colored black before
  //    unsafe layout change.
  if (!ObjectMarking::GreyToBlack<kAtomicity>(obj, marking_state(obj))) {
    DCHECK(IsFixedArrayWithProgressBar(obj) || obj->IsJSObject() ||
           obj->IsString());
  }
  DCHECK(ObjectMarking::IsBlack<kAtomicity>(obj, marking_state(obj)));
  WhiteToGreyAndPush(map);
  IncrementalMarkingMarkingVisitor visitor(heap()->mark_compact_collector());
  visitor.Visit(map, obj);
}

void IncrementalMarking::ProcessBlackAllocatedObject(HeapObject* obj) {
  if (IsMarking() &&
      ObjectMarking::IsBlack<kAtomicity>(obj, marking_state(obj))) {
    RevisitObject(obj);
  }
}

void IncrementalMarking::RevisitObject(HeapObject* obj) {
  DCHECK(IsMarking());
  DCHECK(FLAG_concurrent_marking ||
         ObjectMarking::IsBlack<kAtomicity>(obj, marking_state(obj)));
  Page* page = Page::FromAddress(obj->address());
  if ((page->owner() != nullptr) && (page->owner()->identity() == LO_SPACE)) {
    page->ResetProgressBar();
  }
  Map* map = obj->map();
  WhiteToGreyAndPush(map);
  IncrementalMarkingMarkingVisitor visitor(heap()->mark_compact_collector());
  visitor.Visit(map, obj);
}

intptr_t IncrementalMarking::ProcessMarkingWorklist(
    intptr_t bytes_to_process, ForceCompletionAction completion) {
  intptr_t bytes_processed = 0;
  while (bytes_processed < bytes_to_process || completion == FORCE_COMPLETION) {
    HeapObject* obj = marking_worklist()->Pop();
    if (obj == nullptr) break;
    // Left trimming may result in white, grey, or black filler objects on the
    // marking deque. Ignore these objects.
    if (obj->IsFiller()) {
      DCHECK(!ObjectMarking::IsImpossible<kAtomicity>(obj, marking_state(obj)));
      continue;
    }

    Map* map = obj->map();
    int size = obj->SizeFromMap(map);
    unscanned_bytes_of_large_object_ = 0;
    VisitObject(map, obj, size);
    bytes_processed += size - unscanned_bytes_of_large_object_;
  }
  // Report all found wrappers to the embedder. This is necessary as the
  // embedder could potentially invalidate wrappers as soon as V8 is done
  // with its incremental marking processing. Any cached wrappers could
  // result in broken pointers at this point.
  heap_->local_embedder_heap_tracer()->RegisterWrappersWithRemoteTracer();
  return bytes_processed;
}


void IncrementalMarking::Hurry() {
  // A scavenge may have pushed new objects on the marking deque (due to black
  // allocation) even in COMPLETE state. This may happen if scavenges are
  // forced e.g. in tests. It should not happen when COMPLETE was set when
  // incremental marking finished and a regular GC was triggered after that
  // because should_hurry_ will force a full GC.
  if (!marking_worklist()->IsEmpty()) {
    double start = 0.0;
    if (FLAG_trace_incremental_marking) {
      start = heap_->MonotonicallyIncreasingTimeInMs();
      if (FLAG_trace_incremental_marking) {
        heap()->isolate()->PrintWithTimestamp("[IncrementalMarking] Hurry\n");
      }
    }
    // TODO(gc) hurry can mark objects it encounters black as mutator
    // was stopped.
    ProcessMarkingWorklist(0, FORCE_COMPLETION);
    SetState(COMPLETE);
    if (FLAG_trace_incremental_marking) {
      double end = heap_->MonotonicallyIncreasingTimeInMs();
      double delta = end - start;
      if (FLAG_trace_incremental_marking) {
        heap()->isolate()->PrintWithTimestamp(
            "[IncrementalMarking] Complete (hurry), spent %d ms.\n",
            static_cast<int>(delta));
      }
    }
  }

  Object* context = heap_->native_contexts_list();
  while (!context->IsUndefined(heap_->isolate())) {
    // GC can happen when the context is not fully initialized,
    // so the cache can be undefined.
    HeapObject* cache = HeapObject::cast(
        Context::cast(context)->get(Context::NORMALIZED_MAP_CACHE_INDEX));
    if (!cache->IsUndefined(heap_->isolate())) {
      // Mark the cache black if it is grey.
      bool ignored =
          ObjectMarking::GreyToBlack<kAtomicity>(cache, marking_state(cache));
      USE(ignored);
    }
    context = Context::cast(context)->next_context_link();
  }
}


void IncrementalMarking::Stop() {
  if (IsStopped()) return;
  if (FLAG_trace_incremental_marking) {
    int old_generation_size_mb =
        static_cast<int>(heap()->PromotedSpaceSizeOfObjects() / MB);
    int old_generation_limit_mb =
        static_cast<int>(heap()->old_generation_allocation_limit() / MB);
    heap()->isolate()->PrintWithTimestamp(
        "[IncrementalMarking] Stopping: old generation %dMB, limit %dMB, "
        "overshoot %dMB\n",
        old_generation_size_mb, old_generation_limit_mb,
        Max(0, old_generation_size_mb - old_generation_limit_mb));
  }

  SpaceIterator it(heap_);
  while (it.has_next()) {
    Space* space = it.next();
    if (space == heap_->new_space()) {
      space->RemoveAllocationObserver(&new_generation_observer_);
    } else {
      space->RemoveAllocationObserver(&old_generation_observer_);
    }
  }

  IncrementalMarking::set_should_hurry(false);
  if (IsMarking()) {
    PatchIncrementalMarkingRecordWriteStubs(heap_,
                                            RecordWriteStub::STORE_BUFFER_ONLY);
    DeactivateIncrementalWriteBarrier();
  }
  heap_->isolate()->stack_guard()->ClearGC();
  SetState(STOPPED);
  is_compacting_ = false;
  FinishBlackAllocation();
}


void IncrementalMarking::Finalize() {
  Hurry();
  Stop();
}


void IncrementalMarking::FinalizeMarking(CompletionAction action) {
  DCHECK(!finalize_marking_completed_);
  if (FLAG_trace_incremental_marking) {
    heap()->isolate()->PrintWithTimestamp(
        "[IncrementalMarking] requesting finalization of incremental "
        "marking.\n");
  }
  request_type_ = FINALIZATION;
  if (action == GC_VIA_STACK_GUARD) {
    heap_->isolate()->stack_guard()->RequestGC();
  }
}


void IncrementalMarking::MarkingComplete(CompletionAction action) {
  SetState(COMPLETE);
  // We will set the stack guard to request a GC now.  This will mean the rest
  // of the GC gets performed as soon as possible (we can't do a GC here in a
  // record-write context).  If a few things get allocated between now and then
  // that shouldn't make us do a scavenge and keep being incremental, so we set
  // the should-hurry flag to indicate that there can't be much work left to do.
  set_should_hurry(true);
  if (FLAG_trace_incremental_marking) {
    heap()->isolate()->PrintWithTimestamp(
        "[IncrementalMarking] Complete (normal).\n");
  }
  request_type_ = COMPLETE_MARKING;
  if (action == GC_VIA_STACK_GUARD) {
    heap_->isolate()->stack_guard()->RequestGC();
  }
}


void IncrementalMarking::Epilogue() {
  was_activated_ = false;
  finalize_marking_completed_ = false;
  incremental_marking_finalization_rounds_ = 0;
}

double IncrementalMarking::AdvanceIncrementalMarking(
    double deadline_in_ms, CompletionAction completion_action,
    ForceCompletionAction force_completion, StepOrigin step_origin) {
  HistogramTimerScope incremental_marking_scope(
      heap_->isolate()->counters()->gc_incremental_marking());
  TRACE_EVENT0("v8", "V8.GCIncrementalMarking");
  TRACE_GC(heap_->tracer(), GCTracer::Scope::MC_INCREMENTAL);
  DCHECK(!IsStopped());
  DCHECK_EQ(
      0, heap_->local_embedder_heap_tracer()->NumberOfCachedWrappersToTrace());

  double remaining_time_in_ms = 0.0;
  intptr_t step_size_in_bytes = GCIdleTimeHandler::EstimateMarkingStepSize(
      kStepSizeInMs,
      heap()->tracer()->IncrementalMarkingSpeedInBytesPerMillisecond());

  const bool incremental_wrapper_tracing =
      state_ == MARKING && FLAG_incremental_marking_wrappers &&
      heap_->local_embedder_heap_tracer()->InUse();
  do {
    if (incremental_wrapper_tracing && trace_wrappers_toggle_) {
      TRACE_GC(heap()->tracer(),
               GCTracer::Scope::MC_INCREMENTAL_WRAPPER_TRACING);
      const double wrapper_deadline =
          heap_->MonotonicallyIncreasingTimeInMs() + kStepSizeInMs;
      if (!heap_->local_embedder_heap_tracer()
               ->ShouldFinalizeIncrementalMarking()) {
        heap_->local_embedder_heap_tracer()->Trace(
            wrapper_deadline, EmbedderHeapTracer::AdvanceTracingActions(
                                  EmbedderHeapTracer::ForceCompletionAction::
                                      DO_NOT_FORCE_COMPLETION));
      }
    } else {
      Step(step_size_in_bytes, completion_action, force_completion,
           step_origin);
    }
    trace_wrappers_toggle_ = !trace_wrappers_toggle_;
    remaining_time_in_ms =
        deadline_in_ms - heap()->MonotonicallyIncreasingTimeInMs();
  } while (remaining_time_in_ms >= kStepSizeInMs && !IsComplete() &&
           !marking_worklist()->IsEmpty());
  return remaining_time_in_ms;
}


void IncrementalMarking::FinalizeSweeping() {
  DCHECK(state_ == SWEEPING);
  if (heap_->mark_compact_collector()->sweeping_in_progress() &&
      (!FLAG_concurrent_sweeping ||
       !heap_->mark_compact_collector()->sweeper().AreSweeperTasksRunning())) {
    heap_->mark_compact_collector()->EnsureSweepingCompleted();
  }
  if (!heap_->mark_compact_collector()->sweeping_in_progress()) {
    StartMarking();
  }
}

size_t IncrementalMarking::StepSizeToKeepUpWithAllocations() {
  // Update bytes_allocated_ based on the allocation counter.
  size_t current_counter = heap_->OldGenerationAllocationCounter();
  bytes_allocated_ += current_counter - old_generation_allocation_counter_;
  old_generation_allocation_counter_ = current_counter;
  return bytes_allocated_;
}

size_t IncrementalMarking::StepSizeToMakeProgress() {
  // We increase step size gradually based on the time passed in order to
  // leave marking work to standalone tasks. The ramp up duration and the
  // target step count are chosen based on benchmarks.
  const int kRampUpIntervalMs = 300;
  const size_t kTargetStepCount = 128;
  const size_t kTargetStepCountAtOOM = 16;
  size_t oom_slack = heap()->new_space()->Capacity() + 64 * MB;

  if (heap()->IsCloseToOutOfMemory(oom_slack)) {
    return heap()->PromotedSpaceSizeOfObjects() / kTargetStepCountAtOOM;
  }

  size_t step_size = Max(initial_old_generation_size_ / kTargetStepCount,
                         IncrementalMarking::kAllocatedThreshold);
  double time_passed_ms =
      heap_->MonotonicallyIncreasingTimeInMs() - start_time_ms_;
  double factor = Min(time_passed_ms / kRampUpIntervalMs, 1.0);
  return static_cast<size_t>(factor * step_size);
}

void IncrementalMarking::AdvanceIncrementalMarkingOnAllocation() {
  // Code using an AlwaysAllocateScope assumes that the GC state does not
  // change; that implies that no marking steps must be performed.
  if (heap_->gc_state() != Heap::NOT_IN_GC || !FLAG_incremental_marking ||
      (state_ != SWEEPING && state_ != MARKING) || heap_->always_allocate()) {
    return;
  }

  size_t bytes_to_process =
      StepSizeToKeepUpWithAllocations() + StepSizeToMakeProgress();

  if (bytes_to_process >= IncrementalMarking::kAllocatedThreshold) {
    // The first step after Scavenge will see many allocated bytes.
    // Cap the step size to distribute the marking work more uniformly.
    size_t max_step_size = GCIdleTimeHandler::EstimateMarkingStepSize(
        kMaxStepSizeInMs,
        heap()->tracer()->IncrementalMarkingSpeedInBytesPerMillisecond());
    bytes_to_process = Min(bytes_to_process, max_step_size);

    size_t bytes_processed = 0;
    if (bytes_marked_ahead_of_schedule_ >= bytes_to_process) {
      // Steps performed in tasks have put us ahead of schedule.
      // We skip processing of marking dequeue here and thus
      // shift marking time from inside V8 to standalone tasks.
      bytes_marked_ahead_of_schedule_ -= bytes_to_process;
      bytes_processed = bytes_to_process;
    } else {
      HistogramTimerScope incremental_marking_scope(
          heap_->isolate()->counters()->gc_incremental_marking());
      TRACE_EVENT0("v8", "V8.GCIncrementalMarking");
      TRACE_GC(heap_->tracer(), GCTracer::Scope::MC_INCREMENTAL);
      bytes_processed = Step(bytes_to_process, GC_VIA_STACK_GUARD,
                             FORCE_COMPLETION, StepOrigin::kV8);
    }
    bytes_allocated_ -= Min(bytes_allocated_, bytes_processed);
  }
}

size_t IncrementalMarking::Step(size_t bytes_to_process,
                                CompletionAction action,
                                ForceCompletionAction completion,
                                StepOrigin step_origin) {
  double start = heap_->MonotonicallyIncreasingTimeInMs();

  if (state_ == SWEEPING) {
    TRACE_GC(heap_->tracer(), GCTracer::Scope::MC_INCREMENTAL_SWEEPING);
    FinalizeSweeping();
  }

  size_t bytes_processed = 0;
  if (state_ == MARKING) {
    if (FLAG_trace_incremental_marking && FLAG_trace_concurrent_marking &&
        FLAG_trace_gc_verbose) {
      marking_worklist()->Print();
    }
    bytes_processed = ProcessMarkingWorklist(bytes_to_process);
    if (step_origin == StepOrigin::kTask) {
      bytes_marked_ahead_of_schedule_ += bytes_processed;
    }

    if (marking_worklist()->IsEmpty()) {
      if (heap_->local_embedder_heap_tracer()
              ->ShouldFinalizeIncrementalMarking()) {
        if (completion == FORCE_COMPLETION ||
            IsIdleMarkingDelayCounterLimitReached()) {
          if (!finalize_marking_completed_) {
            FinalizeMarking(action);
          } else {
            MarkingComplete(action);
          }
        } else {
          IncrementIdleMarkingDelayCounter();
        }
      } else {
        heap_->local_embedder_heap_tracer()->NotifyV8MarkingWorklistWasEmpty();
      }
    }
  }
  if (FLAG_concurrent_marking) {
    heap_->concurrent_marking()->RescheduleTasksIfNeeded();
  }

  double end = heap_->MonotonicallyIncreasingTimeInMs();
  double duration = (end - start);
  // Note that we report zero bytes here when sweeping was in progress or
  // when we just started incremental marking. In these cases we did not
  // process the marking deque.
  heap_->tracer()->AddIncrementalMarkingStep(duration, bytes_processed);
  if (FLAG_trace_incremental_marking) {
    heap_->isolate()->PrintWithTimestamp(
        "[IncrementalMarking] Step %s %zu bytes (%zu) in %.1f\n",
        step_origin == StepOrigin::kV8 ? "in v8" : "in task", bytes_processed,
        bytes_to_process, duration);
  }
  return bytes_processed;
}


bool IncrementalMarking::IsIdleMarkingDelayCounterLimitReached() {
  return idle_marking_delay_counter_ > kMaxIdleMarkingDelayCounter;
}


void IncrementalMarking::IncrementIdleMarkingDelayCounter() {
  idle_marking_delay_counter_++;
}


void IncrementalMarking::ClearIdleMarkingDelayCounter() {
  idle_marking_delay_counter_ = 0;
}

}  // namespace internal
}  // namespace v8
