/****************************************************************************
 * drivers/sensors/lis2dh.c
 *
 * LIS2DH accelerometer driver
 *
 *   Copyright (C) 2014 Haltian Ltd. All rights reserved.
 *   Authors: Timo Voutilainen <timo.voutilainen@haltian.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <poll.h>
#include <debug.h>

#include <nuttx/fs/fs.h>
#include <nuttx/kmalloc.h>
#include <nuttx/random.h>
#include <nuttx/i2c.h>

#include <nuttx/sensors/lis2dh.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LIS2DH_COUNT_INTS

#ifdef CONFIG_DEBUG_LIS2DH
#  define lis2dh_dbg(x, ...)        dbg(x, ##__VA_ARGS__)
#  define lis2dh_lldbg(x, ...)      lldbg(x, ##__VA_ARGS__)
#else
#  define lis2dh_dbg(x, ...)
#  define lis2dh_lldbg(x, ...)
#endif

//#define LIS2DH_SELFTEST
#ifdef LIS2DH_SELFTEST
#define SELFTEST_BUF_SIZE           5
#define SELFTEST_MAX_READ_ATTEMPTS  100
#define SELFTEST_ABS_DIFF_MIN       17
#define SELFTEST_ABS_DIFF_MAX       360
#define SELFTEST_0                  0
#define SELFTEST_1                  1
#endif

/* Other macros */

#define LIS2DH_I2C_RETRIES  10

/****************************************************************************
 * Private Data Types
 ****************************************************************************/
enum interrupts
{
  LIS2DH_INT1 = 1,
  LIS2DH_INT2 = 2
};

struct lis2dh_dev_s
{
  FAR struct i2c_dev_s        *i2c;       /* I2C interface */
  uint8_t                     addr;       /* I2C address */
  FAR struct lis2dh_config_s  *config;    /* Platform specific configuration */
  struct lis2dh_setup         *setup;     /* User defined device operation mode setup */
  struct lis2dh_vector_s      vector_data;/* Latest read data read from lis2dh */
  sem_t                       devsem;     /* Manages exclusive access to this structure */
  bool                        fifo_used;  /* LIS2DH configured to use FIFO */
#ifdef LIS2DH_COUNT_INTS
  volatile int16_t            int_pending;/* Interrupt received but data not read, yet */
#else
  volatile bool               int_pending;/* Interrupt received but data not read, yet */
#endif
#ifndef CONFIG_DISABLE_POLL
  struct pollfd               *fds[CONFIG_LIS2DH_NPOLLWAITERS];
#endif
#ifdef LIS2DH_SELFTEST
  bool                        selftest_mode;
  uint8_t                     selftest_read_count;
#endif
};

/****************************************************************************
 * Private Function prototypes
 ****************************************************************************/

static int               lis2dh_open(FAR struct file *filep);
static int               lis2dh_close(FAR struct file *filep);
static ssize_t           lis2dh_read(FAR struct file *, FAR char *, size_t);
static ssize_t           lis2dh_write(FAR struct file *filep, FAR const char *buffer, size_t buflen);
static int               lis2dh_ioctl(FAR struct file *filep, int cmd, unsigned long arg);
static int               lis2dh_access(FAR struct lis2dh_dev_s *dev, uint8_t subaddr, FAR uint8_t *buf, int length);
static FAR const struct lis2dh_vector_s * lis2dh_get_readings(FAR struct lis2dh_dev_s * dev, bool force_read, int *err);
static int               lis2dh_powerdown(FAR struct lis2dh_dev_s * dev);
static int               lis2dh_poll(FAR struct file *filep, FAR struct pollfd *fds, bool setup);
static void              lis2dh_notify(FAR struct lis2dh_dev_s *priv);
static int               lis2dh_int_handler(int irq, FAR void *context);
static int               lis2dh_setup(FAR struct lis2dh_dev_s * dev, struct lis2dh_setup *new_setup);
static int16_t           lis2dh_raw_to_mg(uint8_t raw_hibyte, uint8_t raw_lobyte);
static int               lis2dh_read_temp(FAR struct lis2dh_dev_s *dev, int16_t *temper);
static int               lis2dh_clear_interrupts(FAR struct lis2dh_dev_s *priv, uint8_t interrupts);
#ifdef LIS2DH_SELFTEST
static int               lis2dh_handle_selftest(FAR struct lis2dh_dev_s *priv);
static int16_t           lis2dh_raw_convert_to_12bit(uint8_t raw_hibyte, uint8_t raw_lobyte);
static FAR const struct  lis2dh_vector_s * lis2dh_get_raw_readings(FAR struct lis2dh_dev_s * dev, bool force_read, int *err);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct file_operations g_lis2dhops =
{
  lis2dh_open,
  lis2dh_close,
  lis2dh_read,
  lis2dh_write,
  0,
  lis2dh_ioctl
#ifndef CONFIG_DISABLE_POLL
  , lis2dh_poll
#endif
};

static struct lis2dh_dev_s *lis2dh_data;

static int lis2dh_who_am_i(FAR struct lis2dh_dev_s *dev, uint8_t *id)
{
  int ret;

  ret = lis2dh_access(dev, ST_LIS2DH_WHOAMI_REG, id, 1);
  if (ret < 0)
    {
      lis2dh_dbg("Cannot read who am i value\n");
      return -EIO;
    }

  return ret;
}

/****************************************************************************
 * Name: lis2dh_open
 *
 * Description:
 *   This function is called whenever the LIS2DH device is opened.
 *
 ****************************************************************************/

static int lis2dh_open(FAR struct file *filep)
{
  FAR struct inode        *inode = filep->f_inode;
  FAR struct lis2dh_dev_s *priv  = inode->i_private;
  uint8_t regval;
  int ret = OK;

  /* Probe device */

  if (lis2dh_access(priv, ST_LIS2DH_WHOAMI_REG, &regval, 1) > 0)
    {
      /* Check chip identification, in the future several more compatible parts
       * may be added here.
       */

      if (regval == ST_LIS2DH_WHOAMI_VALUE)
        {
          priv->config->irq_enable(priv->config, true);
          /* Normal exit point */
          ret = lis2dh_clear_interrupts(priv, LIS2DH_INT1 | LIS2DH_INT2);
          return ret;
        }

      /* Otherwise, we mark an invalid device found at given address */

      ret = -ENODEV;
    }
  else
    {
      /* No response at given address is marked as */

      ret = -EFAULT;
    }

  /* Error exit */

  return ret;
}

/****************************************************************************
 * Name: lis2dh_close
 *
 * Description:
 *   This routine is called when the LIS2DH device is closed.
 *
 ****************************************************************************/

static int lis2dh_close(FAR struct file *filep)
{
  FAR struct inode        *inode = filep->f_inode;
  FAR struct lis2dh_dev_s *priv  = inode->i_private;

  priv->config->irq_enable(priv->config, false);
  return lis2dh_powerdown(priv);
}

/****************************************************************************
 * Name: lis2dh_read
 * Description:
 *   This routine is called when the LIS2DH device is read.
 ****************************************************************************/

static ssize_t lis2dh_read(FAR struct file *filep, FAR char *buffer, size_t buflen)
{
  FAR struct inode              *inode = filep->f_inode;
  FAR struct lis2dh_dev_s       *priv  = inode->i_private;
  const struct lis2dh_vector_s  *results;
  FAR struct lis2dh_result      *ptr;
  int readcount = (buflen - sizeof(struct lis2dh_res_header)) / sizeof(struct lis2dh_vector_s);
  int i;
  uint8_t buf;
  uint8_t int1_src = 0, int2_src = 0;
  irqstate_t flags;
  int err;

  if (buflen <  sizeof(struct lis2dh_result ) ||
      (buflen - sizeof(struct lis2dh_res_header)) % sizeof(struct lis2dh_vector_s) != 0)
    {
      lis2dh_dbg("lis2dh: Illegal amount of bytes to read: %d\n", buflen);
      return -EINVAL;
    }

  flags = irqsave();
#ifdef LIS2DH_COUNT_INTS
  if (priv->int_pending > 0)
    {
      priv->int_pending--;
    }
  DEBUGASSERT(priv->int_pending >= 0 && priv->int_pending < 10)
#else
  priv->int_pending = false;
#endif
  irqrestore(flags);

  /* Set pointer to first measurement data */

  ptr = (FAR struct lis2dh_result *)buffer;

  err = sem_wait(&priv->devsem);
  if (err < 0)
  {
    return -EINTR;
  }

  /* If FIFO is used - read until FIFO is empty - or receive buffer (readcount) is reached */

  for (i = 0; i < readcount; i++)
    {
      results = lis2dh_get_readings(priv, true, &err);
      if (results == NULL)
        {
          lis2dh_dbg("lis2dh: Failed to read xyz\n");
          break;
        }
      if (priv->setup->xy_axis_fixup)
        {
          ptr->measurements[i].x = results->y;
          ptr->measurements[i].y = -results->x;
        }
      else
        {
          ptr->measurements[i].x = results->x;
          ptr->measurements[i].y = results->y;
        }
      ptr->measurements[i].z = results->z;

      if (priv->fifo_used)
        {
          /* Check if there is data left in FIFO */

          if (lis2dh_access(priv, ST_LIS2DH_FIFO_SRC_REG, &buf, 1) != 1)
            {
              lis2dh_dbg("lis2dh: Failed to read FIFO source register\n");
              err = -EIO;
              break;
            }
          if (buf & 0x20)
            {
              /* FIFO is empty - stop reading but reset FIFO first */

              buf =  0x00 | priv->setup->trigger_selection |
                  priv->setup->fifo_trigger_threshold;
              if (lis2dh_access(priv, ST_LIS2DH_FIFO_CTRL_REG, &buf, -1) != 1)
                {
                  lis2dh_dbg("lis2dh: Failed to write FIFO control register\n");
                  err = -EIO;
                  break;
                }

              buf =  priv->setup->fifo_mode | priv->setup->trigger_selection |
                  priv->setup->fifo_trigger_threshold;
              if (lis2dh_access(priv, ST_LIS2DH_FIFO_CTRL_REG, &buf, -1) != 1)
                {
                  lis2dh_dbg("lis2dh: Failed to write FIFO control register\n");
                  err = -EIO;
                  break;
                }
              readcount = 1;
            }

        }
      else
        {
          /* FIFO not used stop reading after first read*/
          readcount = 1;
        }
    }
  ptr->header.meas_count = i;

  /* Make sure interrupt will get cleared (by reading this register) in case of latched configuration */
  buf = 0;
  if (lis2dh_access(priv, ST_LIS2DH_INT1_SRC_REG, &buf, 1) != 1)
    {
      lis2dh_dbg("lis2dh: Failed to read INT1_SRC_REG\n");
      err = -EIO;
    }
  if (buf & 0x40)
    {
      /* Interrupt has happened */

      int1_src = buf;
      ptr->header.int1_occurred = true;
    }
  else
    {
      ptr->header.int1_occurred = false;
    }

  /* Make sure interrupt will get cleared (by reading this register) in case of latched configuration */
  buf = 0;
  if (lis2dh_access(priv, ST_LIS2DH_INT2_SRC_REG, &buf, 1) != 1)
    {
      lis2dh_dbg("lis2dh: Failed to read INT2_SRC_REG\n");
      err = -EIO;
    }
  if (buf & 0x40)
    {
      /* Interrupt has happened */

      int2_src = buf;
      ptr->header.int2_occurred = true;
    }
  else
    {
      ptr->header.int2_occurred = false;
    }
  ptr->header.int1_source = int1_src;
  ptr->header.int2_source = int2_src;

  sem_post(&priv->devsem);

  /* 'err' was just for debugging, we do return partial reads here. */

  return sizeof(ptr->header) + i * sizeof(struct lis2dh_vector_s);
}

/****************************************************************************
 * Name: lis2dh_write
 * Description:
 *   This routine is called when the LIS2DH device is written to.
 ****************************************************************************/

static ssize_t lis2dh_write(FAR struct file *filep, FAR const char *buffer,
    size_t buflen)
{
  DEBUGASSERT(filep != NULL && buffer != NULL && buflen > 0);

  return -ENOSYS;
}

/****************************************************************************
 * Name: lis2dh_ioctl
 * Description:
 *   This routine is called when ioctl function call
 *   for the LIS2DH device is done.
 ****************************************************************************/

static int lis2dh_ioctl(FAR struct file *filep, int cmd, unsigned long arg)
{
  FAR struct inode          *inode = filep->f_inode;
  FAR struct lis2dh_dev_s   *priv  = inode->i_private;
  int ret;
  uint8_t buf;

  DEBUGASSERT(filep != NULL);

  ret = sem_wait(&priv->devsem);
  if (ret < 0)
    {
      return -EINTR;
    }

  ret = OK;
  switch (cmd)
  {
  case SNIOC_WRITESETUP:
    {
      /* Write to the configuration registers. Arg: uint8_t value */

      ret = lis2dh_setup(priv, (struct lis2dh_setup *)arg);
      lis2dh_dbg("lis2dh: conf: %02x ret: %d\n", *(uint8_t*)arg, ret);

      /* Make sure interrupt will get cleared (by reading this register) in case of latched configuration */

      lis2dh_clear_interrupts(priv, LIS2DH_INT1 | LIS2DH_INT2);

#ifdef LIS2DH_SELFTEST
      /* After this, currently selftest is not allowed anymore */

      priv->selftest_mode = false;
      priv->selftest_read_count = 0;
#endif
    }
    break;

  case SNIOC_WRITE_INT1THRESHOLD:
    {
      buf = (uint8_t)arg;

      if (lis2dh_access(priv, ST_LIS2DH_INT1_THS_REG, &buf, -1) != 1)
        {
          lis2dh_dbg("lis2dh: Failed to write INT1_THS_REG\n");
          ret = -EIO;
        }

      lis2dh_clear_interrupts(priv, LIS2DH_INT2);
    }
    break;

  case SNIOC_WRITE_INT2THRESHOLD:
    {
      buf = (uint8_t)arg;

      if (lis2dh_access(priv, ST_LIS2DH_INT2_THS_REG, &buf, -1) != 1)
        {
          lis2dh_dbg("lis2dh: Failed to write INT2_THS_REG\n");
          ret = -EIO;
        }

      lis2dh_clear_interrupts(priv, LIS2DH_INT2);
    }
    break;

  case SNIOC_RESET_HPFILTER:
    {
      /* Read reference register to reset/recalib DC offset for HP filter */

      if (lis2dh_access(priv, ST_LIS2DH_REFERENCE_REG, &buf, 1) != 1)
        {
          lis2dh_dbg("lis2dh: Failed to write reference register\n");
          ret = -EIO;
        }

      lis2dh_clear_interrupts(priv, LIS2DH_INT2);
    }
    break;

  case SNIOC_START_SELFTEST:
#ifdef LIS2DH_SELFTEST
    {
      if (priv->selftest_mode == false)
        {
          ret = -EINVAL;
        }
      else
        {
          lis2dh_clear_interrupts(priv, LIS2DH_INT1 | LIS2DH_INT2);
          ret = lis2dh_handle_selftest(priv);
        }
    }
#else
    {
      ret = -EINVAL;
    }
#endif
    break;

  case SNIOC_READ_TEMP:
    {
      ret = lis2dh_read_temp(priv, (int16_t *)arg);
    }
    break;

  case SNIOC_WHO_AM_I:
    {
      ret = lis2dh_who_am_i(priv, (uint8_t *)arg);
    }
    break;

  default:
    {
      lis2dh_dbg("lis2dh: Unrecognized cmd: %d\n", cmd);
      ret = -ENOTTY;
    }
    break;
  }

  sem_post(&priv->devsem);
  return ret;
}

/****************************************************************************
 * Name: lis2dh_poll
 * Description:
 *   This routine is called during LIS2DH device poll
 ****************************************************************************/

#ifndef CONFIG_DISABLE_POLL
static int lis2dh_poll(FAR struct file *filep, FAR struct pollfd *fds, bool setup)
{
  FAR struct inode *inode;
  FAR struct lis2dh_dev_s *priv;
  int ret = OK;
  int i;

  DEBUGASSERT(filep && fds);
  inode = filep->f_inode;

  DEBUGASSERT(inode && inode->i_private);
  priv = (FAR struct lis2dh_dev_s *)inode->i_private;

  ret = sem_wait(&priv->devsem);
  if (ret < 0)
    {
      return -EINTR;
    }

  if (setup)
    {
      /* Ignore waits that do not include POLLIN */

      if ((fds->events & POLLIN) == 0)
        {
          ret = -EDEADLK;
          goto out;
        }

      /* This is a request to set up the poll.  Find an available
       * slot for the poll structure reference
       */

      for (i = 0; i < CONFIG_LIS2DH_NPOLLWAITERS; i++)
        {
          /* Find an available slot */

          if (!priv->fds[i])
            {
              /* Bind the poll structure and this slot */

              priv->fds[i] = fds;
              fds->priv = &priv->fds[i];
              break;
            }
        }

      if (i >= CONFIG_LIS2DH_NPOLLWAITERS)
        {
          fds->priv = NULL;
          ret = -EBUSY;
          goto out;
        }
      if (priv->int_pending)
        {
          lis2dh_notify(priv);
        }
    }
  else if (fds->priv)
    {
      /* This is a request to tear down the poll. */

      struct pollfd **slot = (struct pollfd **)fds->priv;
      DEBUGASSERT(slot != NULL);

      /* Remove all memory of the poll setup */

      *slot = NULL;
      fds->priv = NULL;
    }

out:
  sem_post(&priv->devsem);
  return ret;
}

static void lis2dh_notify(FAR struct lis2dh_dev_s *priv)
{
  DEBUGASSERT(priv != NULL);

  int i;

  /* If there are threads waiting on poll() for LIS2DH data to become available,
   * then wake them up now.  NOTE: we wake up all waiting threads because we
   * do not know that they are going to do.  If they all try to read the data,
   * then some make end up blocking after all.
   */

  for (i = 0; i < CONFIG_LIS2DH_NPOLLWAITERS; i++)
    {
      struct pollfd *fds = priv->fds[i];
      if (fds)
        {
          fds->revents |= POLLIN;
          lis2dh_lldbg("lis2dh: Report events: %02x\n", fds->revents);
          sem_post(fds->sem);
        }
    }
}
#endif /* !CONFIG_DISABLE_POLL */

/****************************************************************************
 * Name: lis2dh_callback
 *
 * Description:
 *   lis2dh interrupt handler
 *
 ****************************************************************************/

static int lis2dh_int_handler(int irq, FAR void *context)
{
  irqstate_t flags;

  DEBUGASSERT(lis2dh_data);

  flags = irqsave();

#ifdef LIS2DH_COUNT_INTS
  lis2dh_data->int_pending++;
#else
  lis2dh_data->int_pending = true;
#endif

#ifndef CONFIG_DISABLE_POLL
  lis2dh_notify(lis2dh_data);
#endif

  irqrestore(flags);

  return OK;
}

#ifdef LIS2DH_SELFTEST

/****************************************************************************
 * Name: lis2dh_handle_selftest
 *
 * Description:
 *   Handle selftest
 *
 ****************************************************************************/
static int lis2dh_handle_selftest(FAR struct lis2dh_dev_s *priv)
{
  const struct lis2dh_vector_s *results;
  uint8_t i, j, buf;
  int8_t attempt;
  int16_t avg_x_no_st = 0, avg_y_no_st = 0, avg_z_no_st = 0;
  int16_t avg_x_with_st = 0, avg_y_with_st = 0, avg_z_with_st = 0;
  int16_t abs_st_x_value,abs_st_y_value, abs_st_z_value;
  int ret = OK;
  int err = OK;

  lis2dh_powerdown(priv);

  /* Set the control register (23h) to ±2g FS, normal mode with BDU (Block Data Update)
   * and HR (High Resolution) bits enabled.
   */

  buf = 0x88;
  if (lis2dh_access(priv, ST_LIS2DH_CTRL_REG4, &buf, -1) != 1)
    {
      lis2dh_dbg("lis2dh: Failed to write CTRL4 REG for selftest\n");
      ret = -EIO;
      goto out;
    }

  /* Set the control register (20h) to 50Hz ODR (Output Data Rate) with X/Y/Z axis enabled. */

  buf = 0x47;
  if (lis2dh_access(priv, ST_LIS2DH_CTRL_REG1, &buf, -1) != 1)
    {
      lis2dh_dbg("lis2dh: Failed to write CTRL1 REG for selftest\n");
      ret = -EIO;
      goto out;
    }

  /* Dummy reads so that values have stabilized */
  for (i = 0; i < 20; i++)
    {
      results = NULL;
      attempt = SELFTEST_MAX_READ_ATTEMPTS;
      while (results == NULL && --attempt > 0)
        {
          results = lis2dh_get_raw_readings(priv, false, &err);
        }
    }

  for (i = 0; i < SELFTEST_BUF_SIZE; i++)
    {
      results = NULL;
      attempt = SELFTEST_MAX_READ_ATTEMPTS;
      while (results == NULL && --attempt > 0)
        {
          results = lis2dh_get_raw_readings(priv, false, &err);
        }
      avg_x_no_st += results->x;
      avg_y_no_st += results->y;
      avg_z_no_st += results->z;
    }
  avg_x_no_st = avg_x_no_st / SELFTEST_BUF_SIZE;
  avg_y_no_st = avg_y_no_st / SELFTEST_BUF_SIZE;
  avg_z_no_st = avg_z_no_st / SELFTEST_BUF_SIZE;

  for (i = SELFTEST_0; i <= SELFTEST_1; i++)
    {
      /* Enable self-test 0 or 1 at +/-2g FS with BDU and HR bits enabled. */

      buf = (i == SELFTEST_0) ? 0x8A : 0x8C;

      if (lis2dh_access(priv, ST_LIS2DH_CTRL_REG4, &buf, -1) != 1)
        {
          lis2dh_dbg("lis2dh: Failed to write CTRL4 REG for selftest\n");
          ret = -EIO;
          goto out;
        }

      /* Dummy reads so that values have stabilized */
      for (j = 0; j < 10; j++)
        {
          results = NULL;
          attempt = SELFTEST_MAX_READ_ATTEMPTS;
          while (results == NULL && --attempt > 0)
            {
              results = lis2dh_get_raw_readings(priv, false, &err);
            }
        }

      for (j = 0; j < SELFTEST_BUF_SIZE; j++)
        {
          results = NULL;
          attempt = SELFTEST_MAX_READ_ATTEMPTS;
          while (results == NULL && --attempt > 0)
            {
              results = lis2dh_get_raw_readings(priv, false, &err);
            }
          avg_x_with_st += results->x;
          avg_y_with_st += results->y;
          avg_z_with_st += results->z;
        }
      avg_x_with_st = avg_x_with_st / SELFTEST_BUF_SIZE;
      avg_y_with_st = avg_y_with_st / SELFTEST_BUF_SIZE;
      avg_z_with_st = avg_z_with_st / SELFTEST_BUF_SIZE;

      /* FIXME: Why do we need to divide by 2 to get proper results? */

      abs_st_x_value = abs(avg_x_with_st - avg_x_no_st) / 2;
      abs_st_y_value = abs(avg_y_with_st - avg_y_no_st) / 2;
      abs_st_z_value = abs(avg_z_with_st - avg_z_no_st) / 2;

      dbg ("ST %d, ABSX: %d, ABSY: %d, ABSZ: %d\n", i, abs_st_x_value, abs_st_y_value, abs_st_z_value);

      if (abs_st_x_value < SELFTEST_ABS_DIFF_MIN || abs_st_x_value > SELFTEST_ABS_DIFF_MAX ||
          abs_st_y_value < SELFTEST_ABS_DIFF_MIN || abs_st_y_value > SELFTEST_ABS_DIFF_MAX ||
          abs_st_z_value < SELFTEST_ABS_DIFF_MIN || abs_st_z_value > SELFTEST_ABS_DIFF_MAX)
        {
          dbg("Selftest %d fail! Limits (%d <= value <= %d). Results: x: %d, y: %d, z: %d ",
              i,
              SELFTEST_ABS_DIFF_MIN, SELFTEST_ABS_DIFF_MAX,
              abs_st_x_value, abs_st_y_value, abs_st_z_value);
          ret = -EIO;
          goto out;
        }
    }

out:
  lis2dh_powerdown(priv);

  return ret;
}

/****************************************************************************
 * Name: lis2dh_raw_to_mg
 *
 * Description:
 *   Convert raw acceleration value to mg
 *
 * Input Parameters:
 *   raw_hibyte   - Hi byte of raw data
 *   raw_lobyte   - Lo byte of raw data
 *
 * Returned Value:
 *   Returns acceleration value in mg
 ****************************************************************************/
static int16_t lis2dh_raw_convert_to_12bit(uint8_t raw_hibyte, uint8_t raw_lobyte)
{
  int16_t value;

  value = (raw_hibyte << 8) |  raw_lobyte;
  value = value >> 4;

  value &= 0xfff;
  if (value & 0x800)
    {
      value = ~value;
      value &= 0xfff;
      value +=1;
      value = -value;
    }

  return value;
}

/****************************************************************************
 * Name: lis2dh_get_raw_readings
 *
 * Description:
 *   Read X, Y, Z - acceleration values from chip
 *
 * Input Parameters:
 *   dev        - pointer to LIS2DH Private Structure
 *   force_read - Read even if new data is not available (old data)
 *
 * Returned Value:
 *   Returns acceleration vectors (High resolution = 12bit values) on success, NULL otherwise.
 ****************************************************************************/
static FAR const struct lis2dh_vector_s * lis2dh_get_raw_readings(FAR struct lis2dh_dev_s * dev, bool force_read, int *err)
{
  uint8_t retval[6];

  DEBUGASSERT(dev != NULL);

  *err = 0;

  if (force_read == false)
    {
      /* Check if there is new data available */
      if (lis2dh_access(dev, ST_LIS2DH_STATUS_REG, retval, 1) == 1)
        {
          /* If result is not yet ready, return NULL */

          if (!(retval[0] & ST_LIS2DH_SR_ZYXDA))
            {
              lis2dh_dbg("lis2dh: Results were not ready\n");
              *err = -EAGAIN;
              return NULL;
            }
        }
    }

  if (lis2dh_access(dev, ST_LIS2DH_OUT_X_L_REG, retval, 6) == 6)
    {
      dev->vector_data.x = lis2dh_raw_convert_to_12bit(retval[1], retval[0]);
      dev->vector_data.y = lis2dh_raw_convert_to_12bit(retval[3], retval[2]);
      dev->vector_data.z = lis2dh_raw_convert_to_12bit(retval[5], retval[4]);
      return &dev->vector_data;
    }

  lis2dh_dbg("Cannot get raw readings!\n");

  return NULL;
}

#endif /* LIS2DH_SELFTEST */

/****************************************************************************
 * Name: lis2dh_clear_interrupts
 *
 * Description:
 *   Clear interrupts from LIS2DH chip
 *
 ****************************************************************************/
static int lis2dh_clear_interrupts(FAR struct lis2dh_dev_s *priv, uint8_t interrupts)
{
  uint8_t buf;
  int ret = OK;

  if (interrupts & LIS2DH_INT1)
    {
      /* Make sure interrupt will get cleared (by reading this register) in case of latched configuration */
      if (lis2dh_access(priv, ST_LIS2DH_INT1_SRC_REG, &buf, 1) != 1)
        {
          lis2dh_dbg("lis2dh: Failed to read INT1_SRC_REG\n");
          ret = -EIO;
        }
    }

  if (interrupts & LIS2DH_INT2)
    {
      /* Make sure interrupt will get cleared (by reading this register) in case of latched configuration */
      if (lis2dh_access(priv, ST_LIS2DH_INT2_SRC_REG, &buf, 1) != 1)
        {
          lis2dh_dbg("lis2dh: Failed to read INT2_SRC_REG\n");
          ret = -EIO;
        }
    }

  return ret;
}

/****************************************************************************
 * Name: lis2dh_get_readings
 *
 * Description:
 *   Read X, Y, Z - acceleration values from chip
 *
 * Input Parameters:
 *   dev        - pointer to LIS2DH Private Structure
 *   force_read - Read even if new data is not available (old data)
 *
 * Returned Value:
 *   Returns acceleration vectors on success, NULL otherwise.
 ****************************************************************************/
static FAR const struct lis2dh_vector_s * lis2dh_get_readings(FAR struct lis2dh_dev_s * dev, bool force_read, int *err)
{
  uint8_t retval[7];

  DEBUGASSERT(dev != NULL);

  *err = 0;

  if (lis2dh_access(dev, ST_LIS2DH_STATUS_REG, retval, 7) == 7)
    {
      /* If result is not yet ready, return NULL */

      if (!force_read && !(retval[0] & ST_LIS2DH_SR_ZYXDA))
        {
          lis2dh_dbg("lis2dh: Results were not ready\n");
          *err = -EAGAIN;
          return NULL;
        }

      dev->vector_data.x = lis2dh_raw_to_mg(retval[2], retval[1]);
      dev->vector_data.y = lis2dh_raw_to_mg(retval[4], retval[3]);
      dev->vector_data.z = lis2dh_raw_to_mg(retval[6], retval[5]);

      /* Add something to entropy pool. */

      add_sensor_randomness(((uint32_t)dev->vector_data.z << 16)
                            | (dev->vector_data.x ^ dev->vector_data.y));

      return &dev->vector_data;
    }

  return NULL;
}

/****************************************************************************
 * Name: lis2dh_raw_to_mg
 *
 * Description:
 *   Convert raw acceleration value to mg
 *
 * Input Parameters:
 *   raw_hibyte   - Hi byte of raw data
 *   raw_lobyte   - Lo byte of raw data
 *
 * Returned Value:
 *   Returns acceleration value in mg
 ****************************************************************************/
static int16_t lis2dh_raw_to_mg(uint8_t raw_hibyte, uint8_t raw_lobyte)
{
  int16_t value;

  value = (raw_hibyte << 8) |  raw_lobyte;
  //value = value >> 4;
  switch(lis2dh_data->setup->fullscale)
  {
  case ST_LIS2DH_CR4_FULL_SCALE_2G:
  default:
    value = value >> 4;
    break;
  case ST_LIS2DH_CR4_FULL_SCALE_4G:
    value = value >> 3;
    break;
  case ST_LIS2DH_CR4_FULL_SCALE_8G:
    value = value / 62; // FIXME
    break;
  case ST_LIS2DH_CR4_FULL_SCALE_16G:
    value = value / 186; // FIXME
    break;
  }
  value &= 0xfff;
  if (value & 0x800)
    {
      value = ~value;
      value &= 0xfff;
      value +=1;
      value = -value;
    }

  return value;
}

static int lis2dh_read_temp(FAR struct lis2dh_dev_s *dev, int16_t *temper)
{
  int ret;
  uint8_t buf[2] = { 0 };

  ret = lis2dh_access(dev, ST_LIS2DH_OUT_TEMP_L_REG, buf, 2);
  if (ret < 0)
    {
      lis2dh_dbg("Cannot read temperature\n");
      return -EIO;
    }

  *temper = buf[0] | ((int16_t)buf[1] << 8);

  return OK;
}

/****************************************************************************
 * LIS2DH Access with range check
 *
 * Description:
 *   Read or write data via I2C
 *
 * Input Parameters:
 *   dev     LIS2DH Private Structure
 *   subaddr LIS2DH Sub Address
 *   buf     Pointer to buffer, either for read or write access
 *   length  When >0 it denotes read access, when <0 it denotes write access
 *           of -length
 *
 * Returned Value:
 *   Returns actual length of data on success or negated errno.
 *
 ****************************************************************************/

static int lis2dh_access(FAR struct lis2dh_dev_s *dev, uint8_t subaddr,
    FAR uint8_t *buf, int length)
{
  uint16_t flags = 0;
  int retval;
  int retries;

  DEBUGASSERT(dev != NULL && buf != NULL && length != 0);

  if (length > 0)
    {
      flags = I2C_M_READ;
    }
  else
    {
      flags = I2C_M_NORESTART;
      length = -length;
    }

  /* Check valid address ranges and set auto address increment flag */

  if (subaddr == ST_LIS2DH_STATUS_AUX_REG)
    {
      if (length > 1)
        {
          length = 1;
        }
    }
  else if (subaddr >= ST_LIS2DH_OUT_TEMP_L_REG && subaddr < 0x10)
    {
      if (length > (0x10 - subaddr))
        {
          length = 0x10 - subaddr;
        }
    }

  else if (subaddr >= ST_LIS2DH_TEMP_CFG_REG && subaddr <= ST_LIS2DH_ACT_DUR_REG)
    {
      if (length > (ST_LIS2DH_ACT_DUR_REG + 1 - subaddr))
        {
          length = ST_LIS2DH_ACT_DUR_REG + 1 - subaddr;
        }
    }
  else
    {
      return -EFAULT;
    }

  if (length > 1)
    {
      subaddr |= 0x80;
    }

  for (retries = 0; retries < LIS2DH_I2C_RETRIES; retries++)
    {
      /* Create message and send */

      struct i2c_msg_s msgv[2] =
      {
          {
              .addr   = dev->addr,
              .flags  = 0,
              .buffer = &subaddr,
              .length = 1
          },
          {
              .addr   = dev->addr,
              .flags  = flags,
              .buffer = buf,
              .length = length
          }
      };

      retval = I2C_TRANSFER(dev->i2c, msgv, 2);
      if (retval == OK)
        {
          return length;
        }
      else
        {
          /* Some error. Try to reset I2C bus and keep trying. */
#ifdef CONFIG_I2C_RESET
          int ret = up_i2creset(dev->i2c);
          if (ret < 0)
            {
              lis2dh_dbg("up_i2creset failed: %d\n", ret);
              return ret;
            }
#endif
          continue;
        }
    }

  return retval;
}

static int lis2dh_powerdown(FAR struct lis2dh_dev_s * dev)
{
  uint8_t buf = 0;
  int ret = OK;

  if (lis2dh_access(dev, ST_LIS2DH_CTRL_REG1, &buf, -1) != 1)
    {
      lis2dh_dbg("Failed to clear CTRL_REG1\n");
      ret = -EIO;
    }
  return ret;
}

/****************************************************************************
 * LIS2DH Setup
 *
 * Description:
 *   Apply new register setup
 *
 * Input Parameters:
 *   dev        - pointer to LIS2DH Private Structure
 *   new_setup  - pointer to new setup data to be configured
 *
 * Returned Value:
 *   Returns OK on success, ERROR otherwise.
 *
 ****************************************************************************/

static int lis2dh_setup(FAR struct lis2dh_dev_s * dev, struct lis2dh_setup *new_setup)
{
  uint8_t value;

  dev->setup = new_setup;

  /* TEMP_CFG_REG */
  value = dev->setup->temp_enable ? (0x3 << 6): 0;
  if (lis2dh_access(dev, ST_LIS2DH_TEMP_CFG_REG, &value, -1) != 1)
    {
      goto error;
    }

  /* CTRL_REG2 */
  value = dev->setup->hpmode | dev->setup->hpcf | dev->setup->fds |
      dev->setup->hpclick | dev->setup->hpis2 | dev->setup->hpis1;
  if (lis2dh_access(dev, ST_LIS2DH_CTRL_REG2, &value, -1) != 1)
    {
      goto error;
    }

  /* CTRL_REG3 */
  value = dev->setup->int1_click_enable | dev->setup->int1_aoi_enable |
      dev->setup->int2_aoi_enable | dev->setup->int1_drdy_enable |
      dev->setup->int2_drdy_enable | dev->setup->int_wtm_enable |
      dev->setup->int_overrun_enable;
  if (lis2dh_access(dev, ST_LIS2DH_CTRL_REG3, &value, -1) != 1)
    {
      goto error;
    }

  /* CTRL_REG4 */
  value = dev->setup->bdu | dev->setup->endian | dev->setup->fullscale |
      dev->setup->high_resolution_enable | dev->setup->selftest |
      dev->setup->spi_mode;
  if (lis2dh_access(dev, ST_LIS2DH_CTRL_REG4, &value, -1) != 1)
    {
      goto error;
    }

  /* CTRL_REG5 */
  value = dev->setup->reboot | dev->setup->fifo_enable | dev->setup->int1_latch |
      dev->setup->int1_4d_enable | dev->setup->int2_latch |
      dev->setup->int2_4d_enable;
  if (lis2dh_access(dev, ST_LIS2DH_CTRL_REG5, &value, -1) != 1)
    {
      goto error;
    }

  /* CTRL_REG6 */
  value = dev->setup->int2_click_enable | dev->setup->int_enable |
      dev->setup->boot_int1_enable | dev->setup->high_low_active;
  if (lis2dh_access(dev, ST_LIS2DH_CTRL_REG6, &value, -1) != 1)
    {
      goto error;
    }

  /* REFERENCE */
  value = dev->setup->reference;
  if (lis2dh_access(dev, ST_LIS2DH_REFERENCE_REG, &value, -1) != 1)
    {
      goto error;
    }

  /* FIFO_CTRL_REG */
  value = dev->setup->fifo_mode | dev->setup->trigger_selection |
      dev->setup->fifo_trigger_threshold;
  if (lis2dh_access(dev, ST_LIS2DH_FIFO_CTRL_REG, &value, -1) != 1)
    {
      goto error;
    }

  /* INT1_CFG */
  value = dev->setup->int1_interrupt_mode | dev->setup->int1_enable_6d | dev->setup->int1_int_z_high_enable |
      dev->setup->int1_int_z_low_enable | dev->setup->int1_int_y_high_enable |
      dev->setup->int1_int_y_low_enable | dev->setup->int1_int_x_high_enable |
      dev->setup->int1_int_x_low_enable;
  if (lis2dh_access(dev, ST_LIS2DH_INT1_CFG_REG, &value, -1) != 1)
    {
      goto error;
    }

  /* INT1_THS */
  value = dev->setup->int1_int_threshold;
  if (lis2dh_access(dev, ST_LIS2DH_INT1_THS_REG, &value, -1) != 1)
    {
      goto error;
    }

  /* INT1_DURATION */
  value = dev->setup->int1_int_duration;
  if (lis2dh_access(dev, ST_LIS2DH_INT1_DUR_REG, &value, -1) != 1)
    {
      goto error;
    }

  /* INT2_CFG */
  value = dev->setup->int2_interrupt_mode | dev->setup->int2_enable_6d | dev->setup->int2_int_z_high_enable |
      dev->setup->int2_int_z_low_enable | dev->setup->int2_int_y_high_enable |
      dev->setup->int2_int_y_low_enable | dev->setup->int2_int_x_high_enable |
      dev->setup->int2_int_x_low_enable;
  if (lis2dh_access(dev, ST_LIS2DH_INT2_CFG_REG, &value, -1) != 1)
    {
      goto error;
    }

  /* INT2_THS */
  value = dev->setup->int2_int_threshold;
  if (lis2dh_access(dev, ST_LIS2DH_INT2_THS_REG, &value, -1) != 1)
    {
      goto error;
    }

  /* INT2_DURATION */
  value = dev->setup->int2_int_duration;
  if (lis2dh_access(dev, ST_LIS2DH_INT2_DUR_REG, &value, -1) != 1)
    {
      goto error;
    }

  /* CLICK_CFG */
  value = dev->setup->z_double_click_enable | dev->setup->z_single_click_enable |
      dev->setup->y_double_click_enable | dev->setup->y_single_click_enable |
      dev->setup->x_double_click_enable | dev->setup->x_single_click_enable;
  if (lis2dh_access(dev, ST_LIS2DH_CLICK_CFG_REG, &value, -1) != 1)
    {
      goto error;
    }

  /* CLICK_THS */
  value = dev->setup->click_threshold;
  if (lis2dh_access(dev, ST_LIS2DH_CLICK_THS_REG, &value, -1) != 1)
    {
      goto error;
    }

  /* TIME_LIMIT */
  value = dev->setup->click_time_limit;
  if (lis2dh_access(dev, ST_LIS2DH_TIME_LIMIT_REG, &value, -1) != 1)
    {
      goto error;
    }

  /* TIME_LATENCY */
  value = dev->setup->click_time_latency;
  if (lis2dh_access(dev, ST_LIS2DH_TIME_LATENCY_REG, &value, -1) != 1)
    {
      goto error;
    }

  /* TIME_WINDOW */
  value = dev->setup->click_time_window;
  if (lis2dh_access(dev, ST_LIS2DH_TIME_WINDOW_REG, &value, -1) != 1)
    {
      goto error;
    }

  /* CTRL_REG1 */
  value = dev->setup->data_rate | dev->setup->low_power_mode_enable |
      dev->setup->zen | dev->setup->yen | dev->setup->xen;
  if (lis2dh_access(dev, ST_LIS2DH_CTRL_REG1, &value, -1) != 1)
    {
      goto error;
    }

  if (dev->setup->fifo_enable)
    {
      dev->fifo_used = true;
    }
  else
    {
      dev->fifo_used = false;
    }

  return OK;

error:

  /* Setup failed - power down */

  lis2dh_powerdown(dev);
  return -EIO;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int lis2dh_register(FAR const char *devpath,
    FAR struct i2c_dev_s *i2c, uint8_t addr, FAR struct lis2dh_config_s *config)
{
  FAR struct lis2dh_dev_s *priv;
  int ret;

  DEBUGASSERT(devpath != NULL && i2c != NULL && config != NULL);

  /* Initialize the LIS2DH device structure */

  priv = (FAR struct lis2dh_dev_s *)kmm_zalloc(sizeof(struct lis2dh_dev_s));
  if (!priv)
    {
      lis2dh_dbg("lis2dh: Failed to allocate instance\n");
      return -ENOMEM;
    }

  /* Initialize semaphores */

  sem_init(&priv->devsem,  0, 1);    /* Initialize device structure semaphore */

  priv->fifo_used = false;
#ifdef LIS2DH_COUNT_INTS
  priv->int_pending = 0;
#else
  priv->int_pending = false;
#endif

#ifdef LIS2DH_SELFTEST
  /* Selftest is currently supported only right after boot */

  priv->selftest_mode = true;
  priv->selftest_read_count = 0;
#endif

  /* Set static pointer to priv data */

  lis2dh_data = priv;

  priv->i2c    = i2c;
  priv->addr   = addr;
  priv->config = config;

  /* Register the character driver */

  ret = register_driver(devpath, &g_lis2dhops, 0666, priv);
  if (ret < 0)
    {
      lis2dh_dbg("lis2dh: Failed to register driver: %d\n", ret);
      goto errout_with_priv;
    }

  /* Prepare interrupt line and handler. */

  priv->config->irq_clear(config);
  priv->config->irq_attach(config, lis2dh_int_handler);
  priv->config->irq_enable(config, false);

  return OK;

errout_with_priv:
  sem_destroy(&priv->devsem);
  kmm_free(priv);

  return ret;
}

