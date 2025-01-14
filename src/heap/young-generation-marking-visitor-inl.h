// Copyright 2023 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_YOUNG_GENERATION_MARKING_VISITOR_INL_H_
#define V8_HEAP_YOUNG_GENERATION_MARKING_VISITOR_INL_H_

#include "src/common/globals.h"
#include "src/heap/marking-worklist-inl.h"
#include "src/heap/memory-chunk.h"
#include "src/heap/minor-mark-sweep.h"
#include "src/heap/objects-visiting-inl.h"
#include "src/heap/objects-visiting.h"
#include "src/heap/pretenuring-handler-inl.h"
#include "src/heap/young-generation-marking-visitor.h"

namespace v8 {
namespace internal {

template <YoungGenerationMarkingVisitorMode marking_mode>
YoungGenerationMarkingVisitor<marking_mode>::YoungGenerationMarkingVisitor(
    Heap* heap,
    PretenuringHandler::PretenuringFeedbackMap* local_pretenuring_feedback)
    : Parent(heap->isolate()),
      marking_worklists_local_(
          heap->minor_mark_sweep_collector()->marking_worklists(),
          heap->cpp_heap()
              ? CppHeap::From(heap->cpp_heap())->CreateCppMarkingState()
              : MarkingWorklists::Local::kNoCppMarkingState),
      ephemeron_table_list_local_(
          *heap->minor_mark_sweep_collector()->ephemeron_table_list()),
      pretenuring_handler_(heap->pretenuring_handler()),
      local_pretenuring_feedback_(local_pretenuring_feedback),
      shortcut_strings_(heap->CanShortcutStringsDuringGC(
          GarbageCollector::MINOR_MARK_SWEEPER)) {}

template <YoungGenerationMarkingVisitorMode marking_mode>
YoungGenerationMarkingVisitor<marking_mode>::~YoungGenerationMarkingVisitor() {
  PublishWorklists();

  // Flush memory chunk live bytes. Atomics are used for incrementing the live
  // bytes counter of the page, so there is no need to defer flushing to the
  // main thread.
  for (auto& pair : live_bytes_data_) {
    if (pair.first) {
      pair.first->IncrementLiveBytesAtomically(pair.second);
    }
  }
}

template <YoungGenerationMarkingVisitorMode marking_mode>
template <typename T>
int YoungGenerationMarkingVisitor<
    marking_mode>::VisitEmbedderTracingSubClassWithEmbedderTracing(Map map,
                                                                   T object) {
  const int size = VisitJSObjectSubclass(map, object);
  if (!marking_worklists_local_.SupportsExtractWrapper()) return size;
  MarkingWorklists::Local::WrapperSnapshot wrapper_snapshot;
  const bool valid_snapshot =
      marking_worklists_local_.ExtractWrapper(map, object, wrapper_snapshot);
  if (size && valid_snapshot) {
    // Success: The object needs to be processed for embedder references.
    marking_worklists_local_.PushExtractedWrapper(wrapper_snapshot);
  }
  return size;
}

template <YoungGenerationMarkingVisitorMode marking_mode>
int YoungGenerationMarkingVisitor<marking_mode>::VisitJSArrayBuffer(
    Map map, JSArrayBuffer object) {
  object->YoungMarkExtension();
  return VisitEmbedderTracingSubClassWithEmbedderTracing(map, object);
}

template <YoungGenerationMarkingVisitorMode marking_mode>
int YoungGenerationMarkingVisitor<marking_mode>::VisitJSApiObject(
    Map map, JSObject object) {
  return VisitEmbedderTracingSubClassWithEmbedderTracing(map, object);
}

template <YoungGenerationMarkingVisitorMode marking_mode>
int YoungGenerationMarkingVisitor<marking_mode>::
    VisitJSDataViewOrRabGsabDataView(Map map,
                                     JSDataViewOrRabGsabDataView object) {
  return VisitEmbedderTracingSubClassWithEmbedderTracing(map, object);
}

template <YoungGenerationMarkingVisitorMode marking_mode>
int YoungGenerationMarkingVisitor<marking_mode>::VisitJSTypedArray(
    Map map, JSTypedArray object) {
  return VisitEmbedderTracingSubClassWithEmbedderTracing(map, object);
}

template <YoungGenerationMarkingVisitorMode marking_mode>
int YoungGenerationMarkingVisitor<marking_mode>::VisitJSObject(
    Map map, JSObject object) {
  int result = Parent::VisitJSObject(map, object);
  DCHECK_LT(0, result);
  pretenuring_handler_->UpdateAllocationSite(map, object,
                                             local_pretenuring_feedback_);
  return result;
}

template <YoungGenerationMarkingVisitorMode marking_mode>
int YoungGenerationMarkingVisitor<marking_mode>::VisitJSObjectFast(
    Map map, JSObject object) {
  int result = Parent::VisitJSObjectFast(map, object);
  DCHECK_LT(0, result);
  pretenuring_handler_->UpdateAllocationSite(map, object,
                                             local_pretenuring_feedback_);
  return result;
}

template <YoungGenerationMarkingVisitorMode marking_mode>
template <typename T, typename TBodyDescriptor>
int YoungGenerationMarkingVisitor<marking_mode>::VisitJSObjectSubclass(
    Map map, T object) {
  int result =
      Parent::template VisitJSObjectSubclass<T, TBodyDescriptor>(map, object);
  DCHECK_LT(0, result);
  pretenuring_handler_->UpdateAllocationSite(map, object,
                                             local_pretenuring_feedback_);
  return result;
}

template <YoungGenerationMarkingVisitorMode marking_mode>
int YoungGenerationMarkingVisitor<marking_mode>::VisitEphemeronHashTable(
    Map map, EphemeronHashTable table) {
  // Register table with Minor MC, so it can take care of the weak keys later.
  // This allows to only iterate the tables' values, which are treated as strong
  // independently of whether the key is live.
  ephemeron_table_list_local_.Push(table);
  for (InternalIndex i : table->IterateEntries()) {
    ObjectSlot value_slot =
        table->RawFieldOfElementAt(EphemeronHashTable::EntryToValueIndex(i));
    VisitPointer(table, value_slot);
  }
  return EphemeronHashTable::BodyDescriptor::SizeOf(map, table);
}

template <YoungGenerationMarkingVisitorMode marking_mode>
template <typename TSlot>
void YoungGenerationMarkingVisitor<marking_mode>::VisitPointersImpl(
    HeapObject host, TSlot start, TSlot end) {
  for (TSlot slot = start; slot < end; ++slot) {
    if constexpr (marking_mode ==
                  YoungGenerationMarkingVisitorMode::kParallel) {
      VisitObjectViaSlot<ObjectVisitationMode::kPushToWorklist,
                         SlotTreatmentMode::kReadWrite>(slot);
    } else {
      VisitObjectViaSlot<ObjectVisitationMode::kPushToWorklist,
                         SlotTreatmentMode::kReadOnly>(slot);
    }
  }
}

template <YoungGenerationMarkingVisitorMode marking_mode>
template <typename TSlot>
V8_INLINE bool
YoungGenerationMarkingVisitor<marking_mode>::VisitObjectViaSlotInRemeberedSet(
    TSlot slot) {
  if constexpr (marking_mode == YoungGenerationMarkingVisitorMode::kParallel) {
    return VisitObjectViaSlot<ObjectVisitationMode::kVisitDirectly,
                              SlotTreatmentMode::kReadWrite>(slot);
  } else {
    return VisitObjectViaSlot<ObjectVisitationMode::kPushToWorklist,
                              SlotTreatmentMode::kReadOnly>(slot);
  }
}

template <YoungGenerationMarkingVisitorMode marking_mode>
template <typename YoungGenerationMarkingVisitor<
              marking_mode>::ObjectVisitationMode visitation_mode,
          typename YoungGenerationMarkingVisitor<
              marking_mode>::SlotTreatmentMode slot_treatment_mode,
          typename TSlot>
V8_INLINE bool YoungGenerationMarkingVisitor<marking_mode>::VisitObjectViaSlot(
    TSlot slot) {
  typename TSlot::TObject target;
  if constexpr (EnableConcurrentVisitation()) {
    target = slot.Relaxed_Load(ObjectVisitorWithCageBases::cage_base());
  } else {
    target = *slot;
  }
  HeapObject heap_object;
  // Treat weak references as strong.
  if (!target.GetHeapObject(&heap_object)) {
    return false;
  }

#ifdef THREAD_SANITIZER
  if constexpr (EnableConcurrentVisitation()) {
    BasicMemoryChunk::FromHeapObject(heap_object)->SynchronizedHeapLoad();
  }
#endif  // THREAD_SANITIZER

  if (!Heap::InYoungGeneration(heap_object)) {
    return false;
  }

#ifdef V8_MINORMS_STRING_SHORTCUTTING
  if (slot_treatment_mode == SlotTreatmentMode::kReadWrite &&
      !ShortCutStrings(reinterpret_cast<HeapObjectSlot&>(slot), &heap_object)) {
    return false;
  }
#endif  // V8_MINORMS_STRING_SHORTCUTTING

  if (!TryMark(heap_object)) return true;

  if constexpr (EnableConcurrentVisitation()) {
    static_assert(visitation_mode != ObjectVisitationMode::kVisitDirectly);
    marking_worklists_local_.Push(heap_object);
    return true;
  }

  // Maps won't change in the atomic pause, so the map can be read without
  // atomics.
  Map map;
  map = Map::cast(*heap_object->map_slot());
  const VisitorId visitor_id = map->visitor_id();
  // Data-only objects don't require any body descriptor visitation at all and
  // are always visited directly.
  if (Map::ObjectFieldsFrom(visitor_id) == ObjectFields::kDataOnly) {
    const int visited_size = heap_object->SizeFromMap(map);
    IncrementLiveBytesCached(
        MemoryChunk::cast(BasicMemoryChunk::FromHeapObject(heap_object)),
        ALIGN_TO_ALLOCATION_ALIGNMENT(visited_size));
    return true;
  }
  if constexpr (visitation_mode == ObjectVisitationMode::kVisitDirectly) {
    // TODO(omerkatz): Why do I need `Parent::` here?
    const int visited_size = Parent::Visit(map, heap_object);
    if (visited_size) {
      IncrementLiveBytesCached(
          MemoryChunk::cast(BasicMemoryChunk::FromHeapObject(heap_object)),
          ALIGN_TO_ALLOCATION_ALIGNMENT(visited_size));
    }
    return true;
  }
  // Default case: Visit via worklist.
  marking_worklists_local_.Push(heap_object);

  return true;
}

#ifdef V8_MINORMS_STRING_SHORTCUTTING
template <YoungGenerationMarkingVisitorMode marking_mode>
V8_INLINE bool YoungGenerationMarkingVisitor<marking_mode>::ShortCutStrings(
    HeapObjectSlot slot, HeapObject* heap_object) {
  DCHECK_EQ(YoungGenerationMarkingVisitorMode::kParallel, marking_mode);
  if (shortcut_strings_) {
    DCHECK(V8_STATIC_ROOTS_BOOL);
#if V8_STATIC_ROOTS_BOOL
    ObjectSlot map_slot = heap_object->map_slot();
    Address map_address = map_slot.load_map().ptr();
    if (map_address == StaticReadOnlyRoot::kThinOneByteStringMap ||
        map_address == StaticReadOnlyRoot::kThinTwoByteStringMap) {
      DCHECK_EQ(heap_object->map(ObjectVisitorWithCageBases::cage_base())
                    ->visitor_id(),
                VisitorId::kVisitThinString);
      *heap_object = ThinString::cast(*heap_object)->actual();
      // ThinStrings always refer to internalized strings, which are always
      // in old space.
      DCHECK(!Heap::InYoungGeneration(*heap_object));
      slot.StoreHeapObject(*heap_object);
      return false;
    } else if (map_address == StaticReadOnlyRoot::kConsOneByteStringMap ||
               map_address == StaticReadOnlyRoot::kConsTwoByteStringMap) {
      // Not all ConsString are short cut candidates.
      const VisitorId visitor_id =
          heap_object->map(ObjectVisitorWithCageBases::cage_base())
              ->visitor_id();
      if (visitor_id == VisitorId::kVisitShortcutCandidate) {
        ConsString string = ConsString::cast(*heap_object);
        if (static_cast<Tagged_t>(string->second().ptr()) ==
            StaticReadOnlyRoot::kempty_string) {
          *heap_object = string->first();
          slot.StoreHeapObject(*heap_object);
          if (!Heap::InYoungGeneration(*heap_object)) {
            return false;
          }
        }
      }
    }
#endif  // V8_STATIC_ROOTS_BOOL
  }
  return true;
}
#endif  // V8_MINORMS_STRING_SHORTCUTTING

template <YoungGenerationMarkingVisitorMode marking_mode>
V8_INLINE void
YoungGenerationMarkingVisitor<marking_mode>::IncrementLiveBytesCached(
    MemoryChunk* chunk, intptr_t by) {
  DCHECK_IMPLIES(V8_COMPRESS_POINTERS_8GB_BOOL,
                 IsAligned(by, kObjectAlignment8GbHeap));
  const size_t hash = base::hash<MemoryChunk*>()(chunk) & kEntriesMask;
  auto& entry = live_bytes_data_[hash];
  if (entry.first && entry.first != chunk) {
    entry.first->IncrementLiveBytesAtomically(entry.second);
    entry.first = chunk;
    entry.second = 0;
  } else {
    entry.first = chunk;
  }
  entry.second += by;
}

}  // namespace internal
}  // namespace v8

#endif  // V8_HEAP_YOUNG_GENERATION_MARKING_VISITOR_INL_H_
