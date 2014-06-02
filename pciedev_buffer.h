

#ifndef PCIEDEV_BUFFER_H_
#define PCIEDEV_BUFFER_H_

struct pciedev_dev;

/**
 * Structure for storage and manipulation of linked list of dma buffers
 */
struct pciedev_buffer_list 
{
    struct pciedev_dev  *parentDev;
    struct list_head    head;
    struct list_head    *next;
    spinlock_t          lock; // TODO: rename
    wait_queue_head_t   waitQueue;
    int                 shutDownFlag;
};
typedef struct pciedev_buffer_list pciedev_buffer_list;

/**
 * Memory block struct used for dma transfer memory.
 */
struct pciedev_buffer {
    struct list_head    list;           /**< List entry struct. */

    unsigned long       size;           /**< Memory block size in bytes */
    unsigned int        order;          /**< Memory block size in units of kernel page order. */

    unsigned long       kaddr;          /**< Kernel virtual address of memory block. */
    dma_addr_t          dma_handle;     /**< Dma handle for memory block. */

    unsigned long       dma_offset;     /**< DMA data offset in device memory */   
    unsigned long       dma_size;       /**< DMA data size */   
    
    unsigned long       state;
};
typedef struct pciedev_buffer pciedev_buffer;

enum {
    BUFFER_STATE_AVAILABLE = 0,   /**< Buffer is available for next operation */
    BUFFER_STATE_WAITING,         /**< Buffer is waiting for DMA to complete */
};

void pciedev_bufferList_init(pciedev_buffer_list *bufferList, struct pciedev_dev *parentDev);

void pciedev_bufferList_append(pciedev_buffer_list* list, pciedev_buffer* buffer);
void pciedev_bufferList_clear(pciedev_buffer_list* list);
pciedev_buffer* pciedev_bufferList_get_free(pciedev_buffer_list* list);
void pciedev_bufferList_set_free(pciedev_buffer_list* list, pciedev_buffer* block);

pciedev_buffer *pciedev_buffer_create(struct pciedev_dev *dev, unsigned long bufSize);
void pciedev_buffer_destroy(struct pciedev_dev *dev, pciedev_buffer *buffer);



#endif /* PCIEDEV_BUFFER_H_ */
