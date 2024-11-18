// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2022 TsingTeng MicroSystem Co., Ltd.
 */

#include "nfc_driver.h"

/*********** PART0: Global Variables Area ***********/
size_t last_count = 0;
static ktime_t g_pre_write_time;

/*********** PART1: Callback Function Area ***********/
static int irq_trigger_check(struct nfc_info *nfc)
{
    int value;
    value = gpio_get_value(nfc->hw_res.irq_gpio);

    if (value == 1) {
        value = 1; //means irq is triggered
    } else {
        value = 0; //means irq is not triggered
    }

    TMS_DEBUG("State = %d\n", value);
    return value;
}

static int i2c_master_block_recv(struct nfc_info *nfc, uint8_t *buf, size_t count,
                          int timeout)
{
    int ret;

    if (timeout > NFC_CMD_RSP_TIMEOUT_MS) {
        timeout = NFC_CMD_RSP_TIMEOUT_MS;
    }

    if (!gpio_get_value(nfc->hw_res.irq_gpio)) {
        while (1) {
            ret = SUCCESS;
            nfc_enable_irq(nfc);

            if (!gpio_get_value(nfc->hw_res.irq_gpio)) {
                if (timeout) {
                    ret = wait_event_interruptible_timeout(nfc->read_wq, !nfc->irq_enable,
                                                           msecs_to_jiffies(timeout));

                    if (ret <= 0) {
                        TMS_ERR("Wakeup of read work queue timeout\n");
                        return -ETIMEDOUT;
                    }
                } else {
                    ret = wait_event_interruptible(nfc->read_wq, !nfc->irq_enable);

                    if (ret) {
                        TMS_ERR("Wakeup of read work queue failed\n");
                        return ret;
                    }
                }
            }

            nfc_disable_irq(nfc);

            if (gpio_get_value(nfc->hw_res.irq_gpio)) {
                break;
            }

            if (!gpio_get_value(nfc->hw_res.irq_gpio)) {
                TMS_ERR("Can not detect interrupt\n");
                return -EIO;
            }

            if (nfc->release_read) {
                TMS_ERR("Releasing read\n");
                return -EWOULDBLOCK;
            }

            TMS_WARN("Spurious interrupt detected\n");
        }
    } else if (device_may_wakeup(nfc->i2c_dev)) { // if irq pin is already high, just let kernel stay awake
        pm_wakeup_event(nfc->i2c_dev, WAKEUP_SRC_TIMEOUT);
    }

    /* Read data */
    ret = i2c_master_recv(nfc->client, buf, count);
    return ret;
}

static int nfc_ioctl_set_state(struct nfc_info *nfc, unsigned long arg)
{
    int ret = SUCCESS;
    TMS_DEBUG("arg = %lu\n", arg);

    switch (arg) {
    case NFC_DLD_PWR_VEN_OFF:
    case NFCC_POWER_OFF:
        nfc_power_control(nfc, OFF);
        break;

    case NFC_DLD_PWR_VEN_ON:
    case NFCC_POWER_ON:
        nfc_power_control(nfc, ON);
        break;

    case NFC_DLD_PWR_DL_OFF:
    case NFCC_FW_DWNLD_OFF:
        nfc_fw_download_control(nfc, OFF);
        break;

    case NFC_DLD_PWR_DL_ON:
    case NFCC_FW_DWNLD_ON:
        nfc_fw_download_control(nfc, ON);
        break;

    case NFCC_HARD_RESET:
        nfc_hard_reset(nfc);
        break;

    case NFC_DLD_FLUSH:
        /*
         * release blocked user thread waiting for pending read
         */
        if (!mutex_trylock(&nfc->read_mutex)) {
            nfc->release_read = true;
            nfc_disable_irq(nfc);
            wake_up(&nfc->read_wq);
            TMS_DEBUG("Waiting for release of blocked read\n");
            mutex_lock(&nfc->read_mutex);
            nfc->release_read = false;
        }

        mutex_unlock(&nfc->read_mutex);
        break;

    default:
        TMS_ERR("Unknow control arg %lu\n", arg);
        ret = -ENOIOCTLCMD;
        break;
    }

    return ret;
}

/*********** PART2: Operation Function Area ***********/
static long nfc_device_ioctl(struct file *file, unsigned int cmd,
                             unsigned long arg)
{
    int ret;
    struct nfc_info *nfc;
    TMS_DEBUG("cmd = %x arg = %zx\n", cmd, arg);
    nfc = file->private_data;

    if (!nfc) {
        TMS_ERR("NFC device no longer exists\n");
        return -ENODEV;
    }

    switch (cmd) {
    case NFC_IRQ_STATE:
        ret = irq_trigger_check(nfc);
        break;

    case NFC_SET_STATE:
        ret = nfc_ioctl_set_state(nfc, arg);
        break;

    case NFC_SET_ESE_STATE:
        ret = nfc_ioctl_set_ese_state(nfc, arg);
        break;

    case NFC_GET_ESE_STATE:
        ret = nfc_ioctl_get_ese_state(nfc, arg);
        break;

    default:
        TMS_ERR("Unknow control cmd[%x]\n", cmd);
        ret = -ENOIOCTLCMD;
    };

    return ret;
}

int nfc_device_flush(struct file *file, fl_owner_t id)
{
    struct nfc_info *nfc;
    nfc = file->private_data;

    if (!nfc) {
        TMS_ERR("NFC device no longer exists\n");
        return -ENODEV;
    }

    if (!mutex_trylock(&nfc->read_mutex)) {
        nfc->release_read = true;
        nfc_disable_irq(nfc);
        wake_up(&nfc->read_wq);
        TMS_DEBUG("Waiting for release of blocked read\n");
        mutex_lock(&nfc->read_mutex);
        nfc->release_read = false;
    } else {
        TMS_DEBUG("Read thread already released\n");
    }

    mutex_unlock(&nfc->read_mutex);
    return SUCCESS;
}

//static unsigned int nfc_device_poll(struct file *file, poll_table *wait)
//{
//    struct nfc_info *nfc;
//    unsigned int mask = 0;
//    int irqtrge = 0;
//    nfc = file->private_data;
//    /* wait for irq trigger is high */
//    poll_wait(file, &nfc->read_wq, wait);
//    irqtrge = irq_trigger_check(nfc);
//
//    if (irqtrge != 0) {
//        /* signal data avail */
//        mask = POLLIN | POLLRDNORM;
//        nfc_disable_irq(nfc);
//    } else {
//        /* irq trigger is low. Activate ISR */
//        if (!nfc->irq_enable) {
//            TMS_DEBUG("Enable IRQ\n");
//            nfc_enable_irq(nfc);
//        } else {
//            TMS_DEBUG("IRQ already enabled\n");
//        }
//    }
//
//    return mask;
//}

ssize_t nfc_device_read(struct file *file, char __user *buf, size_t count,
                        loff_t *offset)
{
    int ret;
    uint8_t *read_buf;
    bool need2byte = false;
    struct nfc_info *nfc;
    nfc = file->private_data;

    if (!nfc) {
        TMS_ERR("NFC device no longer exists\n");
        return -ENODEV;
    }

    if (count > 0 && count < MAX_BUFFER_SIZE) {
    } else if (count > MAX_BUFFER_SIZE) {
        TMS_WARN("The read bytes[%zu] exceeded the buffer max size, count = %d\n",
                 count, MAX_BUFFER_SIZE);
        count = MAX_BUFFER_SIZE;
    } else {
        TMS_ERR("read error,count = %zu\n", count);
        return -EPERM;
    }

    /* malloc read buffer */
    read_buf = devm_kzalloc(nfc->i2c_dev, count, GFP_DMA | GFP_KERNEL);

    if (!read_buf) {
        return -ENOMEM;
    }

    memset(read_buf, 0x00, count);
    mutex_lock(&nfc->read_mutex);

    if (last_count == 3 && count == 1) {
        TMS_DEBUG("Need read 2 bytes\n");
        need2byte = true;
        ++count;
    }

    if (file->f_flags & O_NONBLOCK) {
        /* Noblock read data mode */
        if (!gpio_get_value(nfc->hw_res.irq_gpio)) {
            TMS_WARN("Read called but no IRQ!\n");
            ret = -EAGAIN;
            goto err_release_read;
        } else {
            /* Noblock read data mode */
            ret = i2c_master_recv(nfc->client, read_buf, count);
        }
    } else {
        /* Block read data mode */
        ret = i2c_master_block_recv(nfc, read_buf, count, 0);
    }

    if (need2byte) {
        --count;
        --ret;
    }

    if (ret != count) {
        TMS_ERR("I2C read failed ret = %d\n", ret);
        goto err_release_read;
    }

    g_pre_write_time = ktime_get_boottime();

    if (copy_to_user(buf, read_buf, ret)) {
        TMS_ERR("Copy to user space failed\n");
        ret = -EFAULT;
        goto err_release_read;
    }

    last_count = count;
    tms_buffer_dump("Rx <-", read_buf, count);
err_release_read:
    mutex_unlock(&nfc->read_mutex);
    devm_kfree(nfc->i2c_dev, read_buf);
    return ret;
}

static ssize_t nfc_device_write(struct file *file, const char __user *buf,
                                size_t count, loff_t *offset)
{
    int ret = -EIO;
    uint8_t *write_buf;
    struct nfc_info *nfc = NULL;
    char wakeup_cmd[1] = {0};
    int wakeup_len = 1;
    int retry_count = 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
    ktime_t elapse_time = { .tv64 = 0 };
#else
    ktime_t elapse_time = 0;
#endif
    ktime_t write_time;
    nfc = file->private_data;

    if (!nfc) {
        TMS_ERR("NFC device no longer exists\n");
        return -ENODEV;
    }

    if (count > 0 && count < MAX_BUFFER_SIZE) {
    } else if (count > MAX_BUFFER_SIZE) {
        TMS_WARN("The write bytes[%zu] exceeded the buffer max size, count = %d\n",
                 count, MAX_BUFFER_SIZE);
        count = MAX_BUFFER_SIZE;
    } else {
        TMS_ERR("write error,count = %zu\n", count);
        return -EPERM;
    }

    /* malloc write buffer */
    write_buf = devm_kzalloc(nfc->i2c_dev, count, GFP_DMA | GFP_KERNEL);

    if (!write_buf) {
        TMS_ERR("Read buffer alloc failed\n");
        return -ENOMEM;
    }

    memset(write_buf, 0x00, count);
    mutex_lock(&nfc->write_mutex);

    if (copy_from_user(write_buf, buf, count)) {
        TMS_ERR("Copy from user space failed\n");
        ret = -EFAULT;
        goto err_release_write;
    }

    if (write_buf[0] != T1_HEAD) {
        write_time = ktime_get_boottime();
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
        elapse_time.tv64 = write_time.tv64 - g_pre_write_time.tv64;
#else
        elapse_time = write_time - g_pre_write_time;
#endif
        // make sure elapse_time is not overflow
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
        if (elapse_time.tv64 < 0 ) {
            elapse_time.tv64 = I2C_ELAPSE_TIMEOUT;
        }
        g_pre_write_time = write_time;
        if (elapse_time.tv64 >= I2C_ELAPSE_TIMEOUT) {
#else
        if (elapse_time < 0) {
            elapse_time = I2C_ELAPSE_TIMEOUT;
        }
        g_pre_write_time = write_time;
        if (elapse_time >= I2C_ELAPSE_TIMEOUT) {
#endif
            TMS_DEBUG("TMS NFC need to send 0x00\n");
            while (++retry_count < MAX_I2C_WAKEUP_TIME) {
                ret = i2c_master_send(nfc->client, wakeup_cmd, wakeup_len);
                usleep_range(I2C_WAKEUP_SLEEP_TIME1, I2C_WAKEUP_SLEEP_TIME2);
                if (ret == wakeup_len) {
                    break;
                }
            }
            if (ret < 0) {
                TMS_ERR("TMS NFC failed to write wakeup_cmd : %d, retry for : %d times\n", ret, retry_count);
            }
        }
    }

    /* Write data */
    ret = i2c_master_send(nfc->client, write_buf, count);

    if (ret != count) {
        TMS_ERR("I2C writer error = %d\n", ret);
        ret = -EIO;
        goto err_release_write;
    }

    tms_buffer_dump("Tx ->", write_buf, count);
err_release_write:
    mutex_unlock(&nfc->write_mutex);
    devm_kfree(nfc->i2c_dev, write_buf);
    return ret;
}

static int nfc_device_open(struct inode *inode, struct file *file)
{
    struct nfc_info *nfc = NULL;

    TMS_DEBUG("Kernel version : %06x, NFC driver version : %s\n", LINUX_VERSION_CODE,
             NFC_VERSION);
    TMS_INFO("NFC device number is %d-%d\n", imajor(inode),
              iminor(inode));
    nfc = nfc_get_data(inode);

    if (!nfc) {
        TMS_ERR("NFC device not exist\n");
        return -ENODEV;
    }

    mutex_lock(&nfc->open_dev_mutex);
    file->private_data = nfc;

    if (nfc->open_dev_count == 0) {
        nfc_fw_download_control(nfc, OFF);
        nfc_enable_irq(nfc);
    }

    nfc->open_dev_count++;
    mutex_unlock(&nfc->open_dev_mutex);
    return SUCCESS;
}

static int nfc_device_close(struct inode *inode, struct file *file)
{
    struct nfc_info *nfc = NULL;
    TMS_INFO("Close NFC device[%d-%d]\n", imajor(inode),
              iminor(inode));
    nfc = nfc_get_data(inode);

    if (!nfc) {
        TMS_ERR("NFC device not exist\n");
        return -ENODEV;
    }

    mutex_lock(&nfc->open_dev_mutex);

    if (nfc->open_dev_count == 1) {
        nfc_disable_irq(nfc);
        nfc_fw_download_control(nfc, OFF);
        TMS_DEBUG("Close all NFC device\n");
    }

    if (nfc->open_dev_count > 0) {
        nfc->open_dev_count--;
    }

    file->private_data = NULL;
    mutex_unlock(&nfc->open_dev_mutex);
    return SUCCESS;
}

static const struct file_operations nfc_fops = {
    .owner          = THIS_MODULE,
    .llseek         = no_llseek,
    .open           = nfc_device_open,
    .release        = nfc_device_close,
    .read           = nfc_device_read,
    .write          = nfc_device_write,
    .flush          = nfc_device_flush,
//    .poll           = nfc_device_poll,
    .unlocked_ioctl = nfc_device_ioctl,
};

/*********** PART3: NFC Driver Start Area ***********/
int nfc_device_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    int ret;
    struct nfc_info *nfc = NULL;
    TMS_INFO("Enter\n");

    /* step1 : alloc nfc_info */
    nfc = nfc_data_alloc(&client->dev, nfc);

    if (nfc == NULL) {
        TMS_ERR("Nfc info alloc memory error\n");
        return -ENOMEM;
    }

    /* step2 : init and binding parameters for easy operate */
    nfc->client             = client;
    nfc->i2c_dev            = &client->dev;
    nfc->irq_enable         = true;
    nfc->irq_wake_up        = false;
    nfc->dev.fops           = &nfc_fops;
    /* step3 : register nfc info*/
    ret = nfc_common_info_init(nfc);

    if (ret) {
        TMS_ERR("Init common nfc device failed\n");
        goto err_free_nfc_malloc;
    }

    /* step4 : I2C function check */
    if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
        TMS_ERR("need I2C_FUNC_I2C\n");
        ret = -ENODEV;
        goto err_free_nfc_info;
    }

    /* step5 : init mutex and queues */
    init_waitqueue_head(&nfc->read_wq);
    mutex_init(&nfc->read_mutex);
    mutex_init(&nfc->write_mutex);
    mutex_init(&nfc->open_dev_mutex);
    spin_lock_init(&nfc->irq_enable_slock);
    /* step6 : register nfc device */
    if (!nfc->tms->registe_device) {
        TMS_ERR("nfc->tms->registe_device is NULL\n");
        ret = -ERROR;
        goto err_destroy_mutex;
    }
    ret = nfc->tms->registe_device(&nfc->dev, nfc);
    if (ret) {
        TMS_ERR("NFC device register failed\n");
        goto err_destroy_mutex;
    }

    /* step7 : register nfc irq */
    ret = nfc_irq_register(nfc);

    if (ret) {
        TMS_ERR("register irq failed\n");
        goto err_unregiste_device;
    }

    nfc_disable_irq(nfc);
    nfc_hard_reset(nfc);
    device_init_wakeup(nfc->i2c_dev, true);
    i2c_set_clientdata(client, nfc);
    TMS_INFO("successfully\n");
    return SUCCESS;

err_unregiste_device:
    if (nfc->tms->unregiste_device) {
        nfc->tms->unregiste_device(&nfc->dev);
    }
err_destroy_mutex:
    mutex_destroy(&nfc->read_mutex);
    mutex_destroy(&nfc->write_mutex);
    mutex_destroy(&nfc->open_dev_mutex);
err_free_nfc_info:
    nfc_gpio_release(nfc);
err_free_nfc_malloc:
    nfc_data_free(&client->dev, nfc);
    TMS_ERR("Failed, ret = %d\n", ret);
    return ret;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6,1,0)
int nfc_device_remove(struct i2c_client *client)
#else
void nfc_device_remove(struct i2c_client *client)
#endif
{
    struct nfc_info *nfc;

    nfc = i2c_get_clientdata(client);

    if (!nfc) {
        TMS_ERR("NFC device no longer exists\n");
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,1,0)
        return -ENODEV;
#else
        return;
#endif
    }

    if (nfc->open_dev_count > 0) {
        TMS_ERR("NFC device is being occupied\n");
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,1,0)
        return -EBUSY;
#else
        return;
#endif
    }

    nfc->tms->ven_enable = false;
    device_init_wakeup(nfc->i2c_dev, false);
    free_irq(nfc->client->irq, nfc);
    mutex_destroy(&nfc->read_mutex);
    mutex_destroy(&nfc->write_mutex);
    mutex_destroy(&nfc->open_dev_mutex);
    nfc_gpio_release(nfc);
    if (nfc->tms->unregiste_device) {
        nfc->tms->unregiste_device(&nfc->dev);
    }
    nfc_data_free(&client->dev, nfc);
    i2c_set_clientdata(client, NULL);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,1,0)
    return SUCCESS;
#endif
}

void nfc_device_shutdown(struct i2c_client *client)
{
    struct nfc_info *nfc;

    nfc = i2c_get_clientdata(client);

    if (!nfc) {
        TMS_ERR("NFC device no longer exists\n");
        return;
    }
    nfc_power_control(nfc, OFF);
    msleep(20); // hard reset guard time
}

int nfc_device_suspend(struct device *device)
{
    struct i2c_client *client;
    struct nfc_info *nfc;

    client = to_i2c_client(device);
    nfc    = i2c_get_clientdata(client);

    if (!nfc) {
        TMS_ERR("NFC device no longer exists\n");
        return -ENODEV;
    }

    if (device_may_wakeup(nfc->i2c_dev) && nfc->irq_enable) {
        if (!enable_irq_wake(nfc->client->irq)) {
            nfc->irq_wake_up = true;
        }
    }

    TMS_INFO("irq_wake_up = %d\n", nfc->irq_wake_up);
    return SUCCESS;
}

int nfc_device_resume(struct device *device)
{
    struct i2c_client *client;
    struct nfc_info *nfc;

    client = to_i2c_client(device);
    nfc    = i2c_get_clientdata(client);

    if (!nfc) {
        TMS_ERR("NFC device no longer exists\n");
        return -ENODEV;
    }

    if (device_may_wakeup(nfc->i2c_dev) && nfc->irq_wake_up) {
        if (!disable_irq_wake(nfc->client->irq)) {
            nfc->irq_wake_up = false;
        }
    }

    TMS_INFO("irq_wake_up = %d\n", nfc->irq_wake_up);
    return SUCCESS;
}

#if !IS_ENABLED(CONFIG_TMS_GUIDE_DEVICE)
static const struct dev_pm_ops nfc_pm_ops = {
    .suspend = nfc_device_suspend,
    .resume  = nfc_device_resume,
};

static const struct i2c_device_id nfc_device_id[] = {
    {NFC_DEVICE, 0 },
    { }
};

static struct of_device_id nfc_match_table[] = {
    { .compatible = NFC_DEVICE, },
    { }
};

static struct i2c_driver nfc_i2c_driver = {
    .probe    = nfc_device_probe,
    .remove   = nfc_device_remove,
    .id_table = nfc_device_id,
    .shutdown = nfc_device_shutdown,
    .driver   = {
        .owner          = THIS_MODULE,
        .name           = NFC_DEVICE,
        .of_match_table = nfc_match_table,
        .pm             = &nfc_pm_ops,
        .probe_type     = PROBE_PREFER_ASYNCHRONOUS,
    },
};

int nfc_driver_init(void)
{
    int ret;
    TMS_INFO("Loading nfc driver\n");
    ret = i2c_add_driver(&nfc_i2c_driver);

    if (ret) {
        TMS_ERR("Unable to add i2c driver, ret = %d\n", ret);
    }

    return ret;
}

void nfc_driver_exit(void)
{
    TMS_INFO("Unloading nfc driver\n");
    i2c_del_driver(&nfc_i2c_driver);
}
#endif

MODULE_DESCRIPTION("TMS NFC Driver");
MODULE_LICENSE("GPL");
