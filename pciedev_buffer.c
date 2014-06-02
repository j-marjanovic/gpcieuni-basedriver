#include <linux/module.h>
#include <linux/fs.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/dma-mapping.h>

#include "pciedev_buffer.h"
#include "pciedev_ufn.h"

void pciedev_bufferList_init(pciedev_buffer_list *bufferList, pciedev_dev *parentDev)
{
    bufferList->parentDev = parentDev;
    spin_lock_init(&bufferList->lock);
    init_waitqueue_head(&bufferList->waitQueue);
    INIT_LIST_HEAD(&bufferList->head);
    bufferList->next = 0;
    bufferList->shutDownFlag = 0;
}
EXPORT_SYMBOL(pciedev_bufferList_init); 

/** 
 * @brief Creates memory buffer and appends it to the list of memory buffers for given device.
 * 
 * This function is reentrant.
 * 
 * @param dev       Target device.
 * @param blkSize   Buffer size.
 * @return void
 */
void pciedev_bufferList_append(pciedev_buffer_list* list, pciedev_buffer* buffer)
{
    spin_lock(&list->lock);
    list_add_tail(&buffer->list, &list->head);
    spin_unlock(&list->lock);
    
    if (list_is_singular(&list->head))
    {
        list->next = list->head.next;
    }
}
EXPORT_SYMBOL(pciedev_bufferList_append);   

/** 
 * @brief Clears all buffers allocated for given device.
 * 
 * This function is reentrant.
 * 
 * @param mdev   Target device
 * @return void
 */
void pciedev_bufferList_clear(pciedev_buffer_list* list)
{
    ulong timeout = HZ/1; // one second timeout
    pciedev_buffer *buffer = 0;
    struct list_head *pos;
    struct list_head *tpos;
    int code;
    
    spin_lock(&list->lock);
    list->shutDownFlag = 1;
    
    list_for_each_safe(pos, tpos, &list->head) 
    {
        buffer = list_entry(pos, struct pciedev_buffer, list);
        code = 1;
        
        while (!test_bit(BUFFER_STATE_AVAILABLE, &buffer->state))
        {
            spin_unlock(&list->lock);
            
            PDEBUG(list->parentDev->name, "pciedev_bufferList_clear(): Waiting for pending operations on buffer to complete...");
            code = wait_event_interruptible_timeout(list->waitQueue, test_bit(BUFFER_STATE_AVAILABLE, &buffer->state) , timeout);
            if (code == 0)
            {
                printk(KERN_ALERT "PCIEDEV(%s): Timeout waiting for pending operations to complete before buffer is deleted. Memory won't be released!", 
                       list->parentDev->name);
            }
            else if (code < 0 )
            {
                printk(KERN_ALERT "PCIEDEV(%s): Interrupted while waiting for pending operations to complete before buffer is deleted. Memory won't be released!", 
                       list->parentDev->name);
            }
            
            spin_lock(&list->lock);
        }
        
        list_del(pos);
        
        if (code > 0)
        {
            pciedev_buffer_destroy(list->parentDev, buffer);
        }
        // else deleting buffer could be dangerous. Better have memory leak than kernel panic.
    }
    spin_unlock(&list->lock);
}
EXPORT_SYMBOL(pciedev_bufferList_clear);   

/**
 *  @brief Allocate memory buffer for DMA transfers.
 * 
 * Allocates and initializes pciedev_buffer structure with corresponding contiguous block of memory that can be 
 * used as target in DMA data tranfer from device. Buffer is mapped for DMA from given PCI device.
 * Buffer size should be power of 2 and at least 4kB. Upper limit is system dependant, but anything bigger than 4MB is 
 * likely going to fail with -ENOMEM.
 * 
 * @param dev       Target PCI device.
 * @param blkSize   Buffer size.
 *
 * @return          On success the allocated memory buffer is returned.
 * @retval -ENOMEM  Allocation failed
 * 
 */
pciedev_buffer *pciedev_buffer_create(struct pciedev_dev *dev, unsigned long bufSize)
{
    pciedev_buffer *buffer;
    unsigned long iter = 0;
    
    PDEBUG(dev->name, "pciedev_buffer_create(bufSize=0x%lx)", bufSize);
    
    // allocate the buffer structure
    buffer = kzalloc(sizeof(pciedev_buffer), GFP_KERNEL);
    if (!buffer)
    {
        return ERR_PTR(-ENOMEM);
    }
    
    // init the buffer structure 
    buffer->order    = get_order(bufSize);
    buffer->size     = 1 << (buffer->order + PAGE_SHIFT);
    set_bit(BUFFER_STATE_AVAILABLE, &buffer->state);
    clear_bit(BUFFER_STATE_WAITING, &buffer->state);
    
    // allocate memory block
    buffer->kaddr = __get_free_pages(GFP_KERNEL|__GFP_DMA, buffer->order);
    if (!buffer->kaddr) 
    {
        printk(KERN_ERR "pciedev_buffer_create(): can't get free pages of order %u\n", buffer->order);
        
        pciedev_buffer_destroy(dev, buffer);
        return ERR_PTR(-ENOMEM);
    }
    
    // reserve allocated memory pages to prevent them from being swapped out
    for (iter = 0; iter < buffer->size; iter += PAGE_SIZE) 
    {
        SetPageReserved(virt_to_page(buffer->kaddr + iter));
    }
    
    // map to pci_dev
    buffer->dma_handle = dma_map_single(&dev->pciedev_pci_dev->dev, (void *)buffer->kaddr, buffer->size, DMA_FROM_DEVICE);
    if (dma_mapping_error(&dev->pciedev_pci_dev->dev, buffer->dma_handle)) 
    {
        printk(KERN_ERR "pciedev_buffer_create(): dma mapping error\n");
        pciedev_buffer_destroy(dev, buffer);
        return ERR_PTR(-ENOMEM);
    }    
    
    printk(KERN_INFO "PCIEDEV(%s): Allocated DMA buffer of size 0x%lx\n", dev->name, buffer->size);
    return buffer;
}
EXPORT_SYMBOL(pciedev_buffer_create);   

/**
 * @brief Free resources allocated by memory buffer. 
 * 
 * @param pciedev  PCI device that target buffer is mapped to
 * @param memblock Target buffer
 * @return void
 */
void pciedev_buffer_destroy(struct pciedev_dev *dev, pciedev_buffer *buffer) 
{
    unsigned long iter = 0;
    
    PDEBUG(dev->name, "pciedev_buffer_destroy(buffer=%p)", buffer);
    
    if (!buffer)
    {
        printk(KERN_ERR "PCIEDEV(%s): Got request to release unallocated buffer!\n", dev->name);
        return;
    }
    
    if (buffer->kaddr) 
    {
        // unmap from pci_dev
        if (buffer->dma_handle != DMA_ERROR_CODE)
        {
            dma_unmap_single(&dev->pciedev_pci_dev->dev, buffer->dma_handle, buffer->size, DMA_FROM_DEVICE);
        }
        
        // release allocated memory pages
        for (iter = 0; iter < buffer->size; iter += PAGE_SIZE) 
        {
            ClearPageReserved(virt_to_page(buffer->kaddr + iter));
        }
        free_pages(buffer->kaddr, buffer->order);
    }
    
    kfree(buffer);
}
EXPORT_SYMBOL(pciedev_buffer_destroy);   

/**
 * @brief Gives free (unused) buffer from the list of allocated buffers.
 * 
 * This function is reentrant. The returned buffer is marked reserved so it won't appear free to any 
 * other thread. 
 * Note that this function may block! If there is no free buffer available it will wait for up to 1
 * second. If no buffer becomes available in this time, the call will fail.
 * 
 * @param mdev     Target device
 * @return         On success a free buffer is returned.
 * @retval -EINTR  Interrupted while waiting for available buffer 
 * @retval -BUSY   Timed out while waiting for available buffer 
 * 
 */
pciedev_buffer* pciedev_bufferList_get_free(pciedev_buffer_list* list)
{
    pciedev_buffer *buffer = 0;
    ulong timeout = HZ/1; // one second timeout
    int code = 0;
    
    spin_lock(&list->lock);
    if (list->shutDownFlag) return ERR_PTR(-EINTR);
        
    buffer = list_entry(list->next, struct pciedev_buffer, list);
    while (!test_bit(BUFFER_STATE_AVAILABLE, &buffer->state))
    {
        spin_unlock(&list->lock);

        PDEBUG(list->parentDev->name, "pciedev_bufferList_get_free(): Waiting... ");
        code = wait_event_interruptible_timeout(list->waitQueue, test_bit(BUFFER_STATE_AVAILABLE, &buffer->state) , timeout);
        if (code == 0)
        {
            return ERR_PTR(-EBUSY);
        }
        else if (code < 0 )
        {
            return ERR_PTR(-EINTR);
        }

        spin_lock(&list->lock);
        if (list->shutDownFlag) return ERR_PTR(-EINTR);
        
        buffer = list_entry(list->next, struct pciedev_buffer, list);
    }
    
    clear_bit(BUFFER_STATE_AVAILABLE, &buffer->state); // Buffer is now reserved
    set_bit(BUFFER_STATE_WAITING, &buffer->state);     // Buffer is now waiting for DMA data
    
    // wrap circular buffers list
    if (list_is_last(list->next, &list->head))
    {
        list->next = list->head.next;
    }
    else
    {
        list->next = list->next->next;
    }
    spin_unlock(&list->lock);
    
    return buffer;
}
EXPORT_SYMBOL(pciedev_bufferList_get_free); 

/**
 * @brief Mark buffer free and wake up any waiters.
 * 
 * This function is reentrant. 
 * 
 * @param mdev     Target device
 * @param buffer   Target buffer
 * @return void
 */
void pciedev_bufferList_set_free(pciedev_buffer_list* list, pciedev_buffer* buffer)
{
    spin_lock(&list->lock);
    
    clear_bit(BUFFER_STATE_WAITING, &buffer->state);
    set_bit(BUFFER_STATE_AVAILABLE, &buffer->state);

    spin_unlock(&list->lock);        
    
    // Wake up any process waiting for free buffers
    wake_up_interruptible(&(list->waitQueue)); 
}
EXPORT_SYMBOL(pciedev_bufferList_set_free); 
