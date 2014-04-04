#include <linux/module.h>
#include <linux/fs.h>	
#include <asm/uaccess.h>

#include "pciedev_ufn.h"
#include "pciedev_io.h"

ssize_t pciedev_read_exp(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    u64        itemsize       = 0;
    ssize_t    retval         = 0;
    int        minor          = 0;
    int        d_num          = 0;
    int        tmp_offset     = 0;
    int        tmp_mode       = 0;
    int        tmp_barx       = 0;
    int        tmp_rsrvd_rw   = 0;
    int        tmp_size_rw    = 0;
    u8         tmp_revision   = 0;
    u32        mem_tmp        = 0;
    int        i              = 0;
    u8         tmp_data_8;
    u16        tmp_data_16;
    u32        tmp_data_32;
    device_rw  reading;
    void*       address;
    void*      pDataBuf          = 0;
    
    struct pciedev_dev *dev = filp->private_data;
    minor = dev->dev_minor;
    d_num = dev->dev_num;
    
    if(!dev->dev_sts){
        printk("PCIEDEV_READ_EXP: NO DEVICE %d\n", dev->dev_num);
        retval = -EFAULT;
        return retval;
    }

    itemsize = sizeof(device_rw);

    if (mutex_lock_interruptible(&dev->dev_mut)){
                    return -ERESTARTSYS;
    }
    if (copy_from_user(&reading, buf, count)) {
            retval = -EFAULT;
            mutex_unlock(&dev->dev_mut);
            return retval;
    }
      
    tmp_mode     = reading.mode_rw;
    if(tmp_mode == RW_INFO){
        pci_read_config_byte(dev->pciedev_pci_dev, PCI_REVISION_ID, &tmp_revision);
        reading.offset_rw = dev->parent_dev->PCIEDEV_DRV_VER_MIN;
        reading.data_rw   = dev->parent_dev->PCIEDEV_DRV_VER_MAJ;
        reading.mode_rw   = tmp_revision;
        reading.barx_rw   = dev->pciedev_all_mems;
        reading.size_rw   = dev->slot_num; /*SLOT NUM*/
        retval            = itemsize;
        if (copy_to_user(buf, &reading, count)) {
             printk(KERN_ALERT "PCIEDEV_READ_EXP 3\n");
             retval = -EFAULT;
             mutex_unlock(&dev->dev_mut);
             retval = 0; /*FIXME this seems wrong here. copy to user failed, so this shoud return an error*/
             return retval;
        }

        mutex_unlock(&dev->dev_mut);
        return retval;
    }
    tmp_offset   = reading.offset_rw;
    tmp_barx     = reading.barx_rw;
    tmp_rsrvd_rw = reading.rsrvd_rw;
    tmp_size_rw  = reading.size_rw;
    
    if ( (tmp_barx < 0) || (tmp_barx >= PCIEDEV_N_BARS) ){
       mutex_unlock(&dev->dev_mut);
       return -EFAULT;
    }

    if(!dev->memmory_base[tmp_barx]){
      mutex_unlock(&dev->dev_mut);
      return -EFAULT;
    }

    address = (void *) dev->memmory_base[tmp_barx];
    /* FIXME: WTF does the -2 do here? This is two bytes down, so in the middle of a 32 bit word! */
    mem_tmp = (dev->mem_base_end[tmp_barx] -2);

    /* again those 2. Is this a copy and paste artefact from some ancient 16 bit VME code? */
    if(tmp_size_rw < 2){
        if(tmp_offset > (mem_tmp -2) || (!address)){
              reading.data_rw   = 0;
              retval            = 0;
        }else{
              switch(tmp_mode){
                case RW_D8:
                    tmp_offset = (tmp_offset/sizeof(u8))*sizeof(u8);
                    tmp_data_8        = ioread8(address + tmp_offset);
                    rmb();
                    reading.data_rw   = tmp_data_8 & 0xFF;
                    retval = itemsize;
                    break;
                case RW_D16:
                    tmp_offset = (tmp_offset/sizeof(u16))*sizeof(u16);
                    tmp_data_16       = ioread16(address + tmp_offset);
                    rmb();
                    reading.data_rw   = tmp_data_16 & 0xFFFF;
                    retval = itemsize;
                    break;
                case RW_D32:
                    tmp_offset = (tmp_offset/sizeof(u32))*sizeof(u32);
                    tmp_data_32       = ioread32(address + tmp_offset);
                    rmb();
                    reading.data_rw   = tmp_data_32 & 0xFFFFFFFF;
                    retval = itemsize;
                    break;
                default:
		  mutex_unlock(&dev->dev_mut);
		  return -EFAULT;
              }
	}

        if (copy_to_user(buf, &reading, count)) {
             retval = -EFAULT;
             mutex_unlock(&dev->dev_mut);
             retval = 0; /*FIXME this seems wrong here. copy to user failed, so this shoud return an error*/
	     /* Jippeee, copy and pasting erros is fun */
             return retval;
        }
    }else{ /*(tmp_size_rw < 2)*/
          switch(tmp_mode){
            case RW_D8:
                if((tmp_offset + tmp_size_rw) > (mem_tmp -2) || (!address)){
                      reading.data_rw   = 0;
                      retval            = 0;
                }else{
                    for(i = 0; i< tmp_size_rw; i++){
                        tmp_data_8        = ioread8(address + tmp_offset + i);
                        rmb();
                        reading.data_rw   = tmp_data_8 & 0xFF;
                        retval = itemsize;
                        if (copy_to_user((buf + i), &reading, 1)) {
                             retval = -EFAULT;
                             mutex_unlock(&dev->dev_mut);
                             retval = 0;/* FIXME Jippeee, copy and pasting erros is fun */
                             return retval;
                        }
                    }
                }
                break;
            case RW_D16:
                if((tmp_offset + tmp_size_rw*2) > (mem_tmp -2) || (!address)){
                      reading.data_rw   = 0;
                      retval            = 0;
                }else{
                    for(i = 0; i< tmp_size_rw; i++){
                        tmp_data_16       = ioread16(address + tmp_offset);
                        rmb();
                        reading.data_rw   = tmp_data_16 & 0xFFFF;
                        retval = itemsize;
                        if (copy_to_user((buf + i*2), &reading, 2)) {
                             retval = -EFAULT;
                             mutex_unlock(&dev->dev_mut);
                             retval = 0;
                             return retval;
                        }
                    }
                }
                break;
            case RW_D32:
                if((tmp_offset + tmp_size_rw*4) > (mem_tmp -2) || (!address)){
                      printk(KERN_ALERT "NO READ SIZE MORE THAN MEM \n");
                      reading.data_rw   = 0;
                      retval            = 0;
                }else{
                    
                    for(i = 0; i< tmp_size_rw; i++){
                        tmp_data_32       = ioread32(address + tmp_offset + i*4);
                        rmb();
                        retval = itemsize;
                        if (copy_to_user((buf + i*4), &tmp_data_32, 4)) {
                             retval = -EFAULT;
                             mutex_unlock(&dev->dev_mut);
                             retval = 0;
                             return retval;
                        }
                    }
/*
                     //pDataBuf = (void *)__get_free_page(GFP_KERNEL | GFP_ATOMIC);
                     pDataBuf = (void *)__get_free_page(GFP_KERNEL );
                     ioread32_rep(address + tmp_offset ,  (void*)pDataBuf, tmp_size_rw);
                     //memcpy_fromio((void*)pDataBuf, (address + tmp_offset), tmp_size_rw*sizeof(int));
                     if (copy_to_user((void*)buf , (void*)pDataBuf, tmp_size_rw*sizeof(int))) {
                             retval = -EFAULT;
                             free_page((ulong)pDataBuf);
                             mutex_unlock(&dev->dev_mut);
                             retval = 0;
                             return retval;
                        }
                     free_page((ulong)pDataBuf);
*/
                     
                }
                break;
            default:
	      mutex_unlock(&dev->dev_mut);
	      return -EFAULT;

          } /* switch(tmp_mode) */

    }/* else (tmp_size_rw < 2) */

    mutex_unlock(&dev->dev_mut);
    return retval;
}
EXPORT_SYMBOL(pciedev_read_exp);

ssize_t pciedev_write_exp(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    device_rw       reading;
    int             itemsize       = 0;
    ssize_t         retval         = 0;
    int             minor          = 0;
    int             d_num          = 0;
    int             tmp_offset     = 0;
    int             tmp_mode       = 0;
    int             tmp_barx       = 0;
    u32             mem_tmp        = 0;
    int             tmp_rsrvd_rw   = 0;
    int             tmp_size_rw    = 0;
    u16             tmp_data_8;
    u16             tmp_data_16;
    u32             tmp_data_32;
    void            *address ;
   
    struct pciedev_dev       *dev = filp->private_data;
    minor = dev->dev_minor;
    d_num = dev->dev_num;
    
    if(!dev->dev_sts){
        printk("PCIEDEV_WRITE_EXP: NO DEVICE %d\n", dev->dev_num);
        retval = -EFAULT;
        return retval;
    }
    
    itemsize = sizeof(device_rw);
    
    if (mutex_lock_interruptible(&dev->dev_mut))
        return -ERESTARTSYS;
    
    if (copy_from_user(&reading, buf, count)) {
        retval = -EFAULT;
        mutex_unlock(&dev->dev_mut);
        return retval;
    }
    
    tmp_mode     = reading.mode_rw;
    tmp_offset   = reading.offset_rw;
    tmp_barx     = reading.barx_rw;
    tmp_data_32  = reading.data_rw & 0xFFFFFFFF;
    tmp_rsrvd_rw = reading.rsrvd_rw;
    tmp_size_rw  = reading.size_rw;
    
    /* check that the bar number is valid */
    if ( (tmp_barx < 0) || (tmp_barx >= PCIEDEV_N_BARS) ){
       mutex_unlock(&dev->dev_mut);
       return -EFAULT;
    }

    /* check that the bar is in use */
    if(!dev->memmory_base[tmp_barx]){
      printk(KERN_ALERT "NO MEM UNDER BAR %u\n", tmp_barx);
      mutex_unlock(&dev->dev_mut);
      return -EFAULT;
    }

    address    = (void *)dev->memmory_base[tmp_barx];
    mem_tmp = (dev->mem_base_end[tmp_barx] -2);
    
    if(tmp_offset > (mem_tmp -2) || (!address)){
        reading.data_rw   = 0;
        retval            = 0;
    }else{
        switch(tmp_mode){
            case RW_D8:
                tmp_offset = (tmp_offset/sizeof(u8))*sizeof(u8);
                tmp_data_8 = tmp_data_32 & 0xFF;
                iowrite8(tmp_data_8, ((void*)(address + tmp_offset)));
                wmb();
                retval = itemsize;
                break;
            case RW_D16:
                tmp_offset = (tmp_offset/sizeof(u16))*sizeof(u16);
                tmp_data_16 = tmp_data_32 & 0xFFFF;
                iowrite16(tmp_data_16, ((void*)(address + tmp_offset)));
                wmb();
                retval = itemsize;
                break;
            case RW_D32:
                tmp_offset = (tmp_offset/sizeof(u32))*sizeof(u32);
                iowrite32(tmp_data_32, ((void*)(address + tmp_offset)));
                wmb();
                retval = itemsize;                
                break;
            default:
		mutex_unlock(&dev->dev_mut);
		return -EFAULT;
        }
    }
    
    mutex_unlock(&dev->dev_mut);
    return retval;
}
EXPORT_SYMBOL(pciedev_write_exp);