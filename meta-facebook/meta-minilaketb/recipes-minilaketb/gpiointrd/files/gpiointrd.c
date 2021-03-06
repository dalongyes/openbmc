/*
 * sensord
 *
 * Copyright 2015-present Facebook. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/un.h>
#include <sys/file.h>
#include <openbmc/kv.h>
#include <openbmc/ipmi.h>
#include <openbmc/pal.h>
#include <openbmc/gpio.h>
#include <facebook/bic.h>
#include <facebook/minilaketb_gpio.h>
#include <facebook/minilaketb_sensor.h>
#include <facebook/minilaketb_common.h>

#define POLL_TIMEOUT -1 /* Forever */
#define MAX_NUM_SLOTS       1

#define GPIO_VAL "/sys/class/gpio/gpio%d/value"

#define SLOT_RECORD_FILE "/tmp/slot%d.rc"

#define HOTSERVICE_SCRPIT "/usr/local/bin/hotservice-reinit.sh"
#define HOTSERVICE_FILE "/tmp/slot%d_reinit"
#define HSLOT_PID  "/tmp/slot%u_reinit.pid"

#define DEBUG_ME_EJECTOR_LOG 1 // Enable log "GPIO_SLOTX_EJECTOR_LATCH_DETECT_N is 1 and SLOT_12v is ON" before mechanism issue is fixed

static uint8_t IsHotServiceStart[MAX_NODES + 1] = {0};
static pthread_mutex_t hsvc_mutex[MAX_NODES + 1];
static struct timespec last_ejector_ts[MAX_NODES + 1];

const static uint8_t gpio_12v[] = { 0, GPIO_P12V_STBY_SLOT1_EN };

typedef struct {
  uint8_t def_val;
  char name[64];
  uint8_t num;
  char log[256];
} def_chk_info;

struct delayed_log {
  useconds_t usec;
  char msg[256];
};

typedef struct {
  uint8_t slot_id;
  uint8_t action;
} hot_service_info;

enum {
  REMOVAl = 0,
  INSERTION = 1,
};

slot_kv_st slot_kv_list[] = {
  // {slot_key, slot_def_val}
  {"pwr_server%d_last_state", "on"},
  {"sysfw_ver_slot%d",         "0"},
  {"slot%d_por_cfg",         "lps"},
  {"slot%d_sensor_health",     "1"},
  {"slot%d_sel_error",         "1"},
  {"slot%d_boot_order",  "0000000"},
  {"slot%d_cpu_ppin",          "0"},
  {"fru%d_restart_cause",      "3"},
};

// Thread for delay event
static void *
delay_log(void *arg)
{
  struct delayed_log* log = (struct delayed_log*)arg;

  pthread_detach(pthread_self());

  if (arg) {
    usleep(log->usec);
    syslog(LOG_CRIT, log->msg);

    free(arg);
  }

  pthread_exit(NULL);
}

static int
read_device(const char *device, int *value) {
  FILE *fp;
  int rc;

  fp = fopen(device, "r");
  if (!fp) {
    int err = errno;
#ifdef DEBUG
    syslog(LOG_INFO, "failed to open device %s", device);
#endif
    return err;
  }

  rc = fscanf(fp, "%d", value);
  fclose(fp);
  if (rc != 1) {
#ifdef DEBUG
    syslog(LOG_INFO, "failed to read device %s", device);
#endif
    return ENOENT;
  } else {
    return 0;
  }
}

static int
write_device(const char *device, const char *value) {
  FILE *fp;
  int rc;

  fp = fopen(device, "w");
  if (!fp) {
    int err = errno;
#ifdef DEBUG
    syslog(LOG_INFO, "failed to open device for write %s", device);
#endif
    return err;
  }

  rc = fputs(value, fp);
  fclose(fp);

  if (rc < 0) {
#ifdef DEBUG
    syslog(LOG_INFO, "failed to write device %s", device);
#endif
    return ENOENT;
  } else {
    return 0;
  }
}

static void log_gpio_change(gpio_poll_st *gp, useconds_t log_delay)
{
  if (log_delay == 0) {
    syslog(LOG_CRIT, "%s: %s - %s\n", gp->value ? "DEASSERT": "ASSERT", gp->name, gp->desc);
  } else {
    pthread_t tid_delay_log;
    struct delayed_log *log = (struct delayed_log *)malloc(sizeof(struct delayed_log));
    if (log) {
      log->usec = log_delay;
      snprintf(log->msg, 256, "%s: %s - %s\n", gp->value ? "DEASSERT" : "ASSERT", gp->name, gp->value);
      if (pthread_create(&tid_delay_log, NULL, delay_log, (void *)log)) {
        free(log);
        log = NULL;
      }
    }
    if (!log) {
      syslog(LOG_CRIT, "%s: %s - %s\n", gp->value ? "DEASSERT": "ASSERT", gp->name, gp->desc);
    }
  }
}

// Generic Event Handler for GPIO changes
static void gpio_event_handle(gpio_poll_st *gp)
{
  char cmd[128] = {0};
  int ret=-1;
  uint8_t slot_id;
  int value;
  int status;
  char vpath[80] = {0};
  char locstr[MAX_VALUE_LEN];
  static bool prsnt_assert[MAX_NODES + 1]={0};
  static pthread_t hsvc_action_tid[MAX_NODES + 1];
  hot_service_info hsvc_info[MAX_NODES + 1];
  struct timespec ts;

  if (gp->gs.gs_gpio == gpio_num("GPIOH5")) { // GPIO_FAN_LATCH_DETECT
    if (gp->value == 1) { // low to high
      syslog(LOG_CRIT, "ASSERT: SLED is not seated");
      memset(cmd, 0, sizeof(cmd));
      sprintf(cmd, "sv stop fscd ; /usr/local/bin/fan-util --set 100");
      system(cmd);
    }
    else { // high to low
      syslog(LOG_CRIT, "DEASSERT: SLED is seated");
      memset(cmd, 0, sizeof(cmd));
      sprintf(cmd, "/etc/init.d/setup-fan.sh ; sv start fscd");
      system(cmd);
    }
  }

  else if (gp->gs.gs_gpio == gpio_num("GPIOO3")) {
    if (pal_get_hand_sw(&slot_id)) {
      slot_id = HAND_SW_BMC;
    }

    slot_id = (slot_id >= HAND_SW_BMC) ? HAND_SW_SERVER1 : (slot_id + 1);
    sprintf(locstr, "%u", slot_id);
    kv_set("spb_hand_sw", locstr, 0, 0);
    syslog(LOG_INFO, "change hand_sw location to FRU %s by button", locstr);
  }
  else if(gp->gs.gs_gpio == gpio_num("GPIOM6")) {
    int reg;
    //set GPIOM5 high
    if (gp->value == 1) {
        sprintf(vpath, GPIO_VAL, 101);
        write_device(vpath, "1");
    }
    else { //set GPIOM5 low
        sprintf(vpath, GPIO_VAL, 101);
        write_device(vpath, "0");
    }
  }
  else if(gp->gs.gs_gpio == gpio_num("GPIOH4")) {
    int reg;
    if (gp->value == 1) {
        syslog(LOG_CRIT, "DEASSERT:GPIOH4 - RST_BMC_PERST_L");
    }
    else {
        syslog(LOG_CRIT, "ASSERT:GPIOH4 - RST_BMC_PERST_L");
    }
  }
}


static gpio_poll_st g_gpios[] = {
  // {{gpio, fd}, edge, gpioValue, call-back function, GPIO description}
  // {{0, 0}, GPIO_EDGE_BOTH,    0, gpio_event_handle, "GPIOH5",  "GPIO_FAN_LATCH_DETECT"},
  // {{0, 0}, GPIO_EDGE_BOTH,    0, gpio_event_handle, "GPIOP0",  "GPIO_SLOT1_EJECTOR_LATCH_DETECT_N"},
  // {{0, 0}, GPIO_EDGE_BOTH,    0, gpio_event_handle, "GPIOP1",  "GPIO_SLOT2_EJECTOR_LATCH_DETECT_N"},
  // {{0, 0}, GPIO_EDGE_BOTH,    0, gpio_event_handle, "GPIOP2",  "GPIO_SLOT3_EJECTOR_LATCH_DETECT_N"},
  // {{0, 0}, GPIO_EDGE_BOTH,    0, gpio_event_handle, "GPIOP3",  "GPIO_SLOT4_EJECTOR_LATCH_DETECT_N"},
  // {{0, 0}, GPIO_EDGE_BOTH,    0, gpio_event_handle, "GPIOZ0",  "GPIO_SLOT1_PRSNT_B_N"},
  // {{0, 0}, GPIO_EDGE_BOTH,    0, gpio_event_handle, "GPIOZ1",  "GPIO_SLOT2_PRSNT_B_N"},
  // {{0, 0}, GPIO_EDGE_BOTH,    0, gpio_event_handle, "GPIOZ2",  "GPIO_SLOT3_PRSNT_B_N"},
  // {{0, 0}, GPIO_EDGE_BOTH,    0, gpio_event_handle, "GPIOZ3",  "GPIO_SLOT4_PRSNT_B_N"},
  // {{0, 0}, GPIO_EDGE_BOTH,    0, gpio_event_handle, "GPIOAA0", "GPIO_SLOT1_PRSNT_N"},
  // {{0, 0}, GPIO_EDGE_BOTH,    0, gpio_event_handle, "GPIOAA1", "GPIO_SLOT2_PRSNT_N"},
  // {{0, 0}, GPIO_EDGE_BOTH,    0, gpio_event_handle, "GPIOAA2", "GPIO_SLOT3_PRSNT_N"},
  // {{0, 0}, GPIO_EDGE_BOTH,    0, gpio_event_handle, "GPIOAA3", "GPIO_SLOT4_PRSNT_N"},
  // {{0, 0}, GPIO_EDGE_FALLING, 0, gpio_event_handle, "GPIOO3",  "GPIO_UART_SEL"},
  {{0, 0}, GPIO_EDGE_BOTH,    0, gpio_event_handle, "GPIOM6",  "FM_CB_SLP3_BUF_N"},
  {{0, 0}, GPIO_EDGE_BOTH,    0, gpio_event_handle, "GPIOH4",  "RST_BMC_PERST_L"},
};

static int g_count = sizeof(g_gpios) / sizeof(gpio_poll_st);

#if DEBUG_ME_EJECTOR_LOG // Enable log "GPIO_SLOTX_EJECTOR_LATCH_DETECT_N is 1 and SLOT_12v is ON" before mechanism issue is fixed
static def_chk_info def_gpio_chk[] = {
  // { default value, gpio name, gpio num, log }
  // { 0, "GPIO_SLOT1_EJECTOR_LATCH_DETECT_N", GPIO_SLOT1_EJECTOR_LATCH_DETECT_N, "GPIO_SLOT1_EJECTOR_LATCH_DETECT_N is \"1\" and SLOT_12v is ON" },
  // { 0, "GPIO_SLOT2_EJECTOR_LATCH_DETECT_N", GPIO_SLOT2_EJECTOR_LATCH_DETECT_N, "GPIO_SLOT2_EJECTOR_LATCH_DETECT_N is \"1\" and SLOT_12v is ON" },
  // { 0, "GPIO_SLOT3_EJECTOR_LATCH_DETECT_N", GPIO_SLOT3_EJECTOR_LATCH_DETECT_N, "GPIO_SLOT3_EJECTOR_LATCH_DETECT_N is \"1\" and SLOT_12v is ON" },
  // { 0, "GPIO_SLOT4_EJECTOR_LATCH_DETECT_N", GPIO_SLOT4_EJECTOR_LATCH_DETECT_N, "GPIO_SLOT4_EJECTOR_LATCH_DETECT_N is \"1\" and SLOT_12v is ON" },
  // { 0, "GPIO_FAN_LATCH_DETECT",             GPIO_FAN_LATCH_DETECT,             "ASSERT: SLED is not seated"                                    },
};

static void default_gpio_check(void) {
  int i;
  int value;
  char vpath[80] = {0};

  for (i=0; i<sizeof(def_gpio_chk)/sizeof(def_chk_info); i++) {
    sprintf(vpath, GPIO_VAL, def_gpio_chk[i].num);
    read_device(vpath, &value);
    if (value != def_gpio_chk[i].def_val)
      syslog(LOG_CRIT, def_gpio_chk[i].log);
  }
}
#endif

static void initial_sync(void)
{
  int value;
  char vpath[80] = {0};

  //sync GPIOM6 to GPIOM5
  sprintf(vpath, GPIO_VAL, 102);
  read_device(vpath, &value);
  sprintf(vpath, GPIO_VAL, 101);
  if(value)
    write_device(vpath, "1");
  else
    write_device(vpath, "0");
}

int
main(int argc, void **argv) {
  int dev, rc, pid_file;
  uint8_t status = 0;
  int i;

  for(i=1 ;i<MAX_NODES + 1; i++)
  {
    pthread_mutex_init(&hsvc_mutex[i], NULL);
    last_ejector_ts[i].tv_sec = 0;
  }

  //initial GPIO value sync
  initial_sync();

#if DEBUG_ME_EJECTOR_LOG // Enable log "GPIO_SLOTX_EJECTOR_LATCH_DETECT_N is 1 and SLOT_12v is ON" before mechanism issue is fixed
  default_gpio_check();
#endif

  pid_file = open("/var/run/gpiointrd.pid", O_CREAT | O_RDWR, 0666);
  rc = flock(pid_file, LOCK_EX | LOCK_NB);
  if(rc) {
    if(EWOULDBLOCK == errno) {
      syslog(LOG_ERR, "Another gpiointrd instance is running...\n");
      exit(-1);
    }
  } else {
    openlog("gpiointrd", LOG_CONS, LOG_DAEMON);
    syslog(LOG_INFO, "gpiointrd: daemon started");

    gpio_poll_open(g_gpios, g_count);
    gpio_poll(g_gpios, g_count, POLL_TIMEOUT);
    gpio_poll_close(g_gpios, g_count);
  }

  for(i=1; i<MAX_NODES + 1; i++)
  {
    pthread_mutex_destroy(&hsvc_mutex[i]);
  }

  return 0;
}
