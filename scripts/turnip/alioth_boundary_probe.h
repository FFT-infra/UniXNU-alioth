// SPDX-License-Identifier: MIT
// Opt-in same-client controls; existing render streams are not rewritten.
#include "tu_clear_blit.h"
static struct tu_device *darwin_boundary_device;
static uint32_t darwin_boundary_stage, darwin_boundary_note_failures;
static const uint32_t kBoundaryNote = 0x50495831;
extern "C" __attribute__((visibility("default"))) bool tu_darwin_boundary_scope(uint32_t stage);
extern "C" __attribute__((visibility("default"))) bool tu_darwin_boundary_probe(uint32_t out[16]);
extern "C" __attribute__((visibility("default"))) void tu_darwin_boundary_stats(uint32_t out[2]);

static bool
boundary_note(uint32_t action, uint32_t fence, struct tu_bo *bo, uint64_t iova, uint32_t bytes)
{
   if (!p_atomic_read(&darwin_boundary_stage) || !dw.open) return false;
   NativeAdrenoAbiNote note = {};
   note.kind = kBoundaryNote;
   note.count = 30;
   note.words[0] = p_atomic_read(&darwin_boundary_stage);
   note.words[1] = action;
   note.words[2] = fence;
   if (bo) {
      note.words[4] = (uint32_t)bo->darwin_handle;
      note.words[5] = (uint32_t)(bo->darwin_handle >> 32);
      note.words[6] = (uint32_t)iova;
      note.words[7] = (uint32_t)(iova >> 32);
      note.words[8] = bytes;
      const uint64_t offset = iova - bo->iova;
      if (bo->map && iova >= bo->iova && offset <= bo->size && bytes <= bo->size-offset) {
         const uint8_t *p = (const uint8_t *)bo->map + offset;
         note.words[9] = MIN2(bytes/4, 16u);
         note.words[10] = (uint32_t)(uintptr_t)p;
         note.words[11] = (uint32_t)((uintptr_t)p >> 32);
         for (unsigned i=0;i<note.words[9];++i) memcpy(&note.words[12+i],p+4*i,4);
      }
   }
   const bool accepted=dw_call(NativeAdrenoMethodNote,&note,sizeof(note),NULL,0)==KERN_SUCCESS;
   if(!accepted)p_atomic_inc(&darwin_boundary_note_failures);
   return accepted;
}

static void
boundary_entries(struct tu_device *dev, const NativeAdrenoAbiSubmitBuffer *entries, unsigned count, uint32_t fence)
{
   if (!p_atomic_read(&darwin_boundary_stage)) return;
   for (unsigned i=0;i<count;++i) {
      struct tu_bo *bo=tu_device_lookup_bo(dev,(uint32_t)entries[i].handle);
      boundary_note(fence?5:2,fence,bo,entries[i].iova,(uint32_t)entries[i].bytes);
   }
}

extern "C" __attribute__((visibility("default"))) bool
tu_darwin_boundary_scope(uint32_t stage)
{
   if (!dw.open || !darwin_boundary_device) return false;
   if (stage) {
      p_atomic_set(&darwin_boundary_stage,stage);
      return boundary_note(1,0,NULL,0,0);
   }
   const bool ok=boundary_note(4,0,NULL,0,0);
   p_atomic_set(&darwin_boundary_stage,0);
   return ok;
}

extern "C" __attribute__((visibility("default"))) void
tu_darwin_boundary_stats(uint32_t out[2])
{
   if(out){out[0]=p_atomic_read(&darwin_boundary_stage);out[1]=p_atomic_read(&darwin_boundary_note_failures);}
}

// out: valid, failed-stage, raw-result/fence/value, TU-result/fence/value,
// expected raw/TU values, target IOVA, seed, CP header, reserved, note failures.
extern "C" __attribute__((visibility("default"))) bool
tu_darwin_boundary_probe(uint32_t out[16])
{
   if (!out) return false;
   memset(out,0,16*sizeof(*out));out[2]=out[4]=out[5]=out[7]=~0u;
   p_atomic_set(&darwin_boundary_note_failures,0);
   struct tu_device *dev=darwin_boundary_device;
   if (!dev || !dw.open) {out[1]=1;return false;}
   struct tu_queue *queue=NULL;unsigned family=0;
   for (;family<TU_MAX_QUEUE_FAMILIES;++family)
      if(dev->queue_count[family] && dev->queues[family][0].type==TU_QUEUE_GFX) {queue=&dev->queues[family][0];break;}
   if(!queue) {out[1]=2;return false;}
   const uint32_t rawValue=0x22446688,fillValue=0x3c3c3c3c;
   out[8]=rawValue;out[9]=fillValue;
   struct tu_bo *target=NULL,*ib=NULL;VkCommandPool pool=VK_NULL_HANDLE;VkCommandBuffer cb=VK_NULL_HANDLE;
   VkResult result=VK_SUCCESS;
   VkDevice device=tu_device_to_handle(dev);
   bool rawSubmitted=false,rawRetired=false,tuSubmitted=false,tuRetired=false;
   pthread_mutex_lock(&dev->submit_mutex);
   out[1]=3;
   result=tu_bo_init_new(dev,&dev->vk.base,&target,4096,(tu_bo_alloc_flags)0,"alioth control target");
   if(result==VK_SUCCESS)result=tu_bo_init_new(dev,&dev->vk.base,&ib,4096,(tu_bo_alloc_flags)0,"alioth control IB");
   if(result==VK_SUCCESS)result=tu_bo_map(dev,target,NULL);
   if(result==VK_SUCCESS)result=tu_bo_map(dev,ib,NULL);
   if(result==VK_SUCCESS && target->map && ib->map) {
      memset(target->map,0xa5,4096);memset(ib->map,0,4096);
      const uint32_t words[]={pm4_pkt7_hdr(CP_MEM_WRITE,3),(uint32_t)target->iova,(uint32_t)(target->iova>>32),rawValue};
      memcpy(ib->map,words,sizeof(words));out[10]=(uint32_t)target->iova;out[11]=(uint32_t)(target->iova>>32);
      memcpy(&out[12],target->map,4);out[13]=words[0];
      p_atomic_set(&darwin_boundary_stage,1);
      boundary_note(1,0,NULL,0,0);
      boundary_note(3,0,target,target->iova,64);
      NativeAdrenoAbiSubmitBuffer entry={ib->darwin_handle,ib->iova,sizeof(words)};
      tu_darwin_submit_rec rec={};uint32_t fence=0;
      out[1]=4;
      result=darwin_submit_chunk(queue,&entry,1,&fence,&rec);
      out[2]=(uint32_t)result;out[3]=fence;rawSubmitted=result==VK_SUCCESS;
      if(rawSubmitted) {
         pthread_mutex_lock(&dw.lock);darwin_note_fence(fence);pthread_mutex_unlock(&dw.lock);
         p_atomic_set(&queue->fence,fence);
         result=dw_wait_fence_locked_free(dev,fence,UINT64_MAX);rawRetired=result==VK_SUCCESS;
         out[2]=(uint32_t)result;
         if(rawRetired) {memcpy(&out[4],target->map,4);out[0]|=1;}
         boundary_note(3,fence,target,target->iova,64);
      }
   }
   pthread_mutex_unlock(&dev->submit_mutex);
   if(result==VK_SUCCESS && rawRetired) {
      p_atomic_set(&darwin_boundary_stage,2);
      boundary_note(1,0,NULL,0,0);
      boundary_note(3,0,target,target->iova+64,64);
      VkCommandPoolCreateInfo pi={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,NULL,VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,family};
      out[1]=5;
      result=dev->vk.dispatch_table.CreateCommandPool(device,&pi,NULL,&pool);
      VkCommandBufferAllocateInfo ai={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,NULL,pool,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};
      if(result==VK_SUCCESS)result=dev->vk.dispatch_table.AllocateCommandBuffers(device,&ai,&cb);
      VkCommandBufferBeginInfo bi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,NULL,VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,NULL};
      if(result==VK_SUCCESS)result=dev->vk.dispatch_table.BeginCommandBuffer(cb,&bi);
      if(result==VK_SUCCESS) {
         // This upstream helper takes an actual TU address, not Metal's synthetic address.
         tu_cmd_fill_buffer_addr(cb,target->iova+64,64,fillValue);
         result=dev->vk.dispatch_table.EndCommandBuffer(cb);
      }
      if(result==VK_SUCCESS) {
         VkSubmitInfo si={};si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
         si.commandBufferCount=1;si.pCommandBuffers=&cb;
         out[1]=6;
         result=dev->vk.dispatch_table.QueueSubmit(tu_queue_to_handle(queue),1,&si,VK_NULL_HANDLE);tuSubmitted=result==VK_SUCCESS;
         if(tuSubmitted) {
            result=dev->vk.dispatch_table.QueueWaitIdle(tu_queue_to_handle(queue));tuRetired=result==VK_SUCCESS;
            out[6]=(uint32_t)p_atomic_read(&queue->fence);
            if(tuRetired) {memcpy(&out[7],(uint8_t *)target->map+64,4);out[0]|=2;}
         }
      }
      out[5]=(uint32_t)result;
      if(tuRetired)boundary_note(3,out[6],target,target->iova+64,64);
   }
   // Retain GPU-visible objects if a submitted command did not retire; never free beneath it.
   const bool safe=(!rawSubmitted || rawRetired) && (!tuSubmitted || tuRetired);
   if(safe) {
      if(pool)dev->vk.dispatch_table.DestroyCommandPool(device,pool,NULL);
      if(ib)tu_bo_finish(dev,ib);
      if(target)tu_bo_finish(dev,target);
   }
   if(out[0]==3)out[1]=0;
   // The caller retains its Metal device and closes stage 3 after the owned fixtures.
   tu_darwin_boundary_scope(3);
   out[15]=p_atomic_read(&darwin_boundary_note_failures);
   return out[0]==3;
}
