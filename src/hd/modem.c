#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/ioctl.h>

#include "hd.h"
#include "hd_int.h"
#include "hddb.h"
#include "modem.h"

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 * modem info
 *
 *
 * Note: what about modem speed?
 *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 */

static struct speeds_s {
  unsigned baud;
  speed_t mask;
} speeds[] = {
  {    1200, B1200    },
  {    1800, B1800    },
  {    2400, B2400    },
  {    4800, B4800    },
  {    9600, B9600    },
  {   19200, B19200   },
  {   38400, B38400   },
  {   57600, B57600   },
  {  115200, B115200  },
  {  230400, B230400  },
  {  460800, B460800  },
  {  500000, B500000  },
  { 1000000, B1000000 },
  { 2000000, B2000000 },
  { 4000000, B4000000 }
};

#define MAX_SPEED	(sizeof speeds / sizeof *speeds)

static void get_serial_modem(hd_data_t* hd_data);
static void guess_modem_name(hd_data_t *hd_data, ser_modem_t *sm);
static void at_cmd(hd_data_t *hd_data, char *at, int raw, int log_it);
static void write_modem(hd_data_t *hd_data, char *msg);
static void read_modem(hd_data_t *hd_data);
static ser_modem_t *add_ser_modem_entry(ser_modem_t **sm, ser_modem_t *new_sm);
static int set_modem_speed(ser_modem_t *sm, unsigned baud);    
static int init_modem(ser_modem_t *mi);
static int is_pnpinfo(ser_modem_t *mi, int ofs);
static unsigned chk4id(ser_modem_t *mi);
static void dump_ser_modem_data(hd_data_t *hd_data);

void hd_scan_modem(hd_data_t *hd_data)
{
  if(!hd_probe_feature(hd_data, pr_modem)) return;

  hd_data->module = mod_modem;

  /* some clean-up */
  remove_hd_entries(hd_data);
  hd_data->ser_modem = NULL;

  PROGRESS(1, 0, "serial");

  get_serial_modem(hd_data);

  if((hd_data->debug & HD_DEB_MODEM)) dump_ser_modem_data(hd_data);
}

void get_serial_modem(hd_data_t *hd_data)
{
  hd_t *hd;
  int i, j, fd;
  unsigned modem_info, baud;
  char buf[4];
  ser_modem_t *sm;
  hd_res_t *res;

  for(hd = hd_data->hd; hd; hd = hd->next) {
    if(hd->base_class == bc_comm && hd->sub_class == sc_com_ser && hd->unix_dev_name) {
      if((fd = open(hd->unix_dev_name, O_RDWR)) >= 0) {
        sm = add_ser_modem_entry(&hd_data->ser_modem, new_mem(sizeof *sm));
        sm->dev_name = new_str(hd->unix_dev_name);
        sm->fd = fd;
        sm->hd_idx = hd->idx;
        sm->do_io = 1;
        init_modem(sm);
      }
    }
  }

  if(!hd_data->ser_modem) return;

  PROGRESS(2, 0, "init");

  usleep(300000);		/* PnP protocol; 200ms seems to be too fast  */
  
  for(sm = hd_data->ser_modem; sm; sm = sm->next) {
    modem_info = TIOCM_DTR | TIOCM_RTS;
    ioctl(sm->fd, TIOCMBIS, &modem_info);
  }

  /* just a quick test if we get a response to an AT command */

  for(i = 0; i < 3; i++) {
    PROGRESS(3, i + 1, "at test");

    for(sm = hd_data->ser_modem; sm; sm = sm->next) {
      if(!sm->is_modem)
        set_modem_speed(sm, i == 0 ? 38400 : i == 1 ? 9600 : 1200);
    }

    at_cmd(hd_data, "AT\r", 1, 1);

    for(sm = hd_data->ser_modem; sm; sm = sm->next) {
      if(strstr(sm->buf, "OK") || strstr(sm->buf, "0")) {
        sm->is_modem = 1;
        sm->do_io = 0;
      }
      sm->buf_len = 0;		/* clear buffer */
    }
  }

  for(sm = hd_data->ser_modem; sm; sm = sm->next) {
    if((sm->do_io = sm->is_modem)) {
      sm->max_baud = sm->cur_baud;
    }
  }

  /* now, go for the maximum speed... */

  PROGRESS(4, 0, "speed");

  for(i = MAX_SPEED - 1; i >= 0; i--) {
    baud = speeds[i].baud;
    for(j = 0, sm = hd_data->ser_modem; sm; sm = sm->next) {
      if(sm->is_modem) {
        if(baud > sm->max_baud) {
          sm->do_io = set_modem_speed(sm, baud) ? 0 : 1;
          if(sm->do_io) j++;
        }
      }
    }

    /* no modems */
    if(!j) continue;

    at_cmd(hd_data, "AT\r", 1, 0);

    for(sm = hd_data->ser_modem; sm; sm = sm->next) {
      if(strstr(sm->buf, "OK") || strstr(sm->buf, "0")) {
        sm->max_baud = sm->cur_baud;
      }
      else {
        sm->do_io = 0;
      }
      sm->buf_len = 0;		/* clear buffer */
    }
  }

  /* now, fix it all up... */
  for(sm = hd_data->ser_modem; sm; sm = sm->next) {
    if(sm->is_modem) {
      set_modem_speed(sm, sm->max_baud);
      sm->do_io = 1;
    }
  }

#if 1
  /* just for testing */
  if((hd_data->debug & HD_DEB_MODEM)) {
    int i;
    int cmds[] = { 0, 1, 2, 3, 6 };
    char at[10];

    PROGRESS(8, 0, "testing");

    for(i = 0; i < sizeof cmds / sizeof *cmds; i++) {
      sprintf(at, "ATI%d\r", cmds[i]);
      at_cmd(hd_data, at, 0, 1);
    }
    at_cmd(hd_data, "AT\r", 0, 1);
  }
#endif

  PROGRESS(5, 0, "pnp id");

  at_cmd(hd_data, "ATI9\r", 1, 1);

  for(sm = hd_data->ser_modem; sm; sm = sm->next) {
    if(sm->is_modem) {
      chk4id(sm);

      if(!sm->user_name) guess_modem_name(hd_data, sm);
    }

    /* reset serial lines */
    tcsetattr(sm->fd, TCSAFLUSH, &sm->tio);
    close(sm->fd);

    if(!sm->is_modem) continue;

    hd = add_hd_entry(hd_data, __LINE__, 0);
    hd->base_class = bc_modem;
    hd->bus = bus_serial;
    hd->unix_dev_name = new_str(sm->dev_name);
    hd->attached_to = sm->hd_idx;
    res = add_res_entry(&hd->res, new_mem(sizeof *res));
    res->baud.type = res_baud;
    res->baud.speed = sm->max_baud;
    if(*sm->pnp_id) {
      strncpy(buf, sm->pnp_id, 3);
      buf[3] = 0;
      hd->vend = name2eisa_id(buf);
      hd->dev = MAKE_ID(TAG_EISA, strtol(sm->pnp_id + 3, NULL, 16));
    }
    hd->serial = new_str(sm->serial);
    if(sm->user_name && hd->dev) {
      add_device_name(hd_data, hd->vend, hd->dev, sm->user_name);
    }
    else if(sm->user_name && sm->vend) {
      if(!hd_find_device_by_name(hd_data, hd->base_class, sm->vend, sm->user_name, &hd->vend, &hd->dev)) {
        hd->dev_name = new_str(sm->user_name);
        hd->vend_name = new_str(sm->vend);
      }
    }
    
  }

}

void guess_modem_name(hd_data_t *hd_data, ser_modem_t *modem)
{
  ser_modem_t *sm;
  str_list_t *sl;
  char *s;

  for(sm = hd_data->ser_modem; sm; sm = sm->next) sm->do_io = 0;

  (sm = modem)->do_io = 1;
  
  at_cmd(hd_data, "ATI3\r", 0, 1);
  sl = sm->at_resp;
  if(sl && !strcmp(sl->str, "ATI3")) sl = sl->next;	/* skip AT cmd echo */

  if(*sl->str == 'U' && strstr(sl->str, "Robotics ")) {
    /* looks like an U.S. Robotics... */

    sm->vend = new_str("U.S. Robotics, Inc.");
    /* strip revision code */
    if((s = strstr(sl->str, " Rev. "))) *s = 0;
    sm->user_name = canon_str(sl->str, strlen(sl->str));

    return;
  }

  at_cmd(hd_data, "ATI2\r", 0, 1);
  sl = sm->at_resp;
  if(sl && !strcmp(sl->str, "ATI2")) sl = sl->next;	/* skip AT cmd echo */

  if(strstr(sl->str, "ZyXEL ")) {
    /* looks like a ZyXEL... */

    sm->vend = new_str("ZyXEL");

    at_cmd(hd_data, "ATI1\r", 0, 1);
    sl = sm->at_resp;
    if(sl && !strcmp(sl->str, "ATI1")) sl = sl->next;
    
    if(sl && sl->next) {
      sl = sl->next;
      if((s = strstr(sl->str, " V "))) *s = 0;
      sm->user_name = canon_str(sl->str, strlen(sl->str));
    }

    return;
  }

}

void at_cmd(hd_data_t *hd_data, char *at, int raw, int log_it)
{
  static unsigned u = 1;
  char *s, *s0;
  ser_modem_t *sm;
  str_list_t *sl;

  for(sm = hd_data->ser_modem; sm; sm = sm->next) {
    if(sm->do_io) sm->buf_len = 0;
  }

  PROGRESS(9, u, "write at cmd");
  write_modem(hd_data, at);
  PROGRESS(9, u, "read at resp");
  read_modem(hd_data);
  PROGRESS(9, u, "read ok");
  u++;

  for(sm = hd_data->ser_modem; sm; sm = sm->next) {
    if(sm->do_io) {
      sm->at_resp = free_str_list(sm->at_resp);
      if(sm->buf_len == 0 || raw) continue;
      s0 = sm->buf;
      while((s = strsep(&s0, "\r\n"))) {
        if(*s) add_str_list(&sm->at_resp, new_str(s));
      }
    }
  }

  if(!(hd_data->debug & HD_DEB_MODEM) || !log_it) return;

  for(sm = hd_data->ser_modem; sm; sm = sm->next) {
    if(sm->do_io) {
      ADD2LOG("%s@%u: %s\n", sm->dev_name, sm->cur_baud, at);
      if(raw) {
        ADD2LOG("  ");
        hexdump(&hd_data->log, 1, sm->buf_len, sm->buf);
        ADD2LOG("\n");
      }
      else {
        for(sl = sm->at_resp; sl; sl = sl->next) ADD2LOG("  %s\n", sl->str);
      }
    }
  }
}


void write_modem(hd_data_t *hd_data, char *msg)
{
  ser_modem_t *sm;
  int i, len = strlen(msg);

  for(sm = hd_data->ser_modem; sm; sm = sm->next) {
    if(sm->do_io) {
      i = write(sm->fd, msg, len);
      if(i != len) {
        ADD2LOG("%s write oops: %d/%d (\"%s\")\n", sm->dev_name, i, len, msg);
      }
    }
  }
}

void read_modem(hd_data_t *hd_data)
{
  int i, sel, fd_max = -1;
  fd_set set, set0;
  struct timeval to;
  ser_modem_t *sm;

  FD_ZERO(&set0);

  for(i = 0, sm = hd_data->ser_modem; sm; sm = sm->next) {
    if(sm->do_io) {
      FD_SET(sm->fd, &set0);
      if(sm->fd > fd_max) fd_max = sm->fd;
      i++;
    }
  }

  if(!i) return;	/* nothing selected */

  for(;;) {
    to.tv_sec = 0; to.tv_usec = 300000;
    set = set0;
    if((sel = select(fd_max + 1, &set, NULL, NULL, &to)) > 0) {
//      fprintf(stderr, "sel: %d\n", sel);
      for(sm = hd_data->ser_modem; sm; sm = sm->next) {
        if(FD_ISSET(sm->fd, &set)) {
          if((i = read(sm->fd, sm->buf + sm->buf_len, sizeof sm->buf - sm->buf_len)) > 0)
            sm->buf_len += i;
//          fprintf(stderr, "%s: got %d\n", sm->dev_name, i);
          if(i <= 0) FD_CLR(sm->fd, &set0);
        }
      }
    }
    else {
      break;
    }
  }

  /* make the strings \000 terminated */
  for(sm = hd_data->ser_modem; sm; sm = sm->next) {
    if(sm->buf_len == sizeof sm->buf) sm->buf_len--;
    sm->buf[sm->buf_len] = 0;
  }
}

int set_modem_speed(ser_modem_t *sm, unsigned baud)
{
  int i;
  speed_t st;
  struct termios tio;

  for(i = 0; i < MAX_SPEED; i++) if(speeds[i].baud == baud) break;

  if(i == MAX_SPEED) return 1;

  if(tcgetattr(sm->fd, &tio)) return errno;

  cfsetospeed(&tio, speeds[i].mask);
  cfsetispeed(&tio, speeds[i].mask);

  if(tcsetattr(sm->fd, TCSAFLUSH, &tio)) return errno;

  /* tcsetattr() returns ok even if it couldn't set the speed... */

  if(tcgetattr(sm->fd, &tio)) return errno;

  st = cfgetospeed(&tio);

  for(i = 0; i < MAX_SPEED; i++) if(speeds[i].mask == st) break;

  if(i == MAX_SPEED) return 2;

  sm->cur_baud = speeds[i].baud;

  return baud == speeds[i].baud ? 0 : 3;
}


int init_modem(ser_modem_t *sm)
{
  struct termios tio;

  if(tcgetattr(sm->fd, &tio)) return errno;
  
  sm->tio = tio;

  tio.c_iflag = IGNBRK | IGNPAR;
  tio.c_oflag = 0;
  tio.c_lflag = 0;
  tio.c_line = 0;
  tio.c_cc[VTIME] = 0;
  tio.c_cc[VMIN] = 1;

  tio.c_cflag = CREAD | CLOCAL | HUPCL | B1200 | CS8;

  if(tcsetattr(sm->fd, TCSAFLUSH, &tio)) return errno;

  return 0;
}


/*
 * Check for a PnP info field starting at ofs;
 * returns either the length of the field or 0 if none was found.
 *
 * the minfo_t struct is updated with the PnP data
 */
int is_pnpinfo(ser_modem_t *mi, int ofs)
{
  int i, j, k, l;
  unsigned char c, *s = mi->buf + ofs, *t;
  int len = mi->buf_len - ofs;
  unsigned serial, class_name, dev_id, user_name;

  if(len <= 0) return 0;

  switch(*s) {
    case 0x08:
      mi->bits = 6; break;
    case 0x28:
      mi->bits = 7; break;
    default:
      return 0;
  }

  if(len < 11) return 0;

  i = 1;

  /* six bit values */
  if((s[i] & ~0x3f) || (s[i + 1] & ~0x3f)) return 0;
  mi->pnp_rev = (s[i] << 6) + s[i + 1];

  /* pnp_rev may *optionally* be given as a string!!! (e.g. "1.0")*/
  if(mi->bits == 7) {
    j = 0;
    if(s[i + 2] < 'A') {
      j++;
      if(s[i + 3] < 'A') j++;
    }
    if(j) {
      if(s[i] < '0' || s[i] > '9') return 0;
      if(s[i + 1] != '.') return 0;
      for(k = 0; k < j; k++)
        if(s[i + 2 + k] < '0' || s[i + 2 + k] > '9') return 0;
      mi->pnp_rev = (s[i] - '0') * 100;
      mi->pnp_rev += s[i + 2] * 10;
      if(j == 2) mi->pnp_rev += s[i + 3];
      i += j;
    }
  }

  i += 2;

  /* the eisa id */
  for(j = 0; j < 7; j++) {
    mi->pnp_id[j] = s[i + j];
    if(mi->bits == 6) mi->pnp_id[j] += 0x20;
  }
  mi->pnp_id[7] = 0;

  i += 7;

  /* now check the id */
  for(j = 0; j < 3; j++) {
    if(
      (mi->pnp_id[j] < 'A' || mi->pnp_id[j] > 'Z') &&
      mi->pnp_id[j] != '_'
    ) return 0;
  }

  for(j = 3; j < 7; j++) {
    if(
      (mi->pnp_id[j] < '0' || mi->pnp_id[j] > '9') &&
      (mi->pnp_id[j] < 'A' || mi->pnp_id[j] > 'F')
    ) return 0;
  }


  if((mi->bits == 6 && s[i] == 0x09) || (mi->bits == 7 && s[i] == 0x29)) {
    return i + 1;
  }

  if((mi->bits != 6 || s[i] != 0x3c) && (mi->bits != 7 || s[i] != 0x5c)) {
    return 0;
  }

  /* parse extended info */
  serial = class_name = dev_id = user_name = 0;
  for(j = 0; i < len; i++) {
    if((mi->bits == 6 && s[i] == 0x09) || (mi->bits == 7 && s[i] == 0x29)) {
      
      if(serial) for(k = serial; k < len; k++) {
        c = s[k];
        if(mi->bits == 6) c += 0x20;
        if(c == '\\') break;
        str_printf(&mi->serial, -1, "%c", c);
      }

      if(class_name) for(k = class_name; k < len; k++) {
        c = s[k];
        if(mi->bits == 6) c += 0x20;
        if(c == '\\') break;
        str_printf(&mi->class_name, -1, "%c", c);
      }

      if(dev_id) for(k = dev_id; k < len; k++) {
        c = s[k];
        if(mi->bits == 6) c += 0x20;
        if(c == '\\') break;
        str_printf(&mi->dev_id, -1, "%c", c);
      }

      if(user_name) {
        for(k = user_name; k < len; k++) {
          c = s[k];
          if(mi->bits == 6) c += 0x20;
          if(c == '\\' || c == ')') break;
          str_printf(&mi->user_name, -1, "%c", c);
        }
        if(mi->user_name && (l = strlen(mi->user_name)) >= 2) {
          /* skip *optional*(!!!) 2 char checksum */
          t = mi->user_name + l - 2;
          if(
            ((t[0] >= '0' && t[0] <= '9') || (t[0] >= 'A' && t[0] <= 'F')) &&
            ((t[1] >= '0' && t[1] <= '9') || (t[1] >= 'A' && t[1] <= 'F'))
          ) {
            /* OK, *might* be a hex number... */
            mi->user_name[l - 2] = 0;

            /*
             * A better check would be to look for the complete name string
             * in the output from another AT command, e.g AT3, AT6 or AT11.
             * If it's there -> no checksum field.
             */
          }
        }
      }

      return i + 1;
    }

    if(((mi->bits == 6 && s[i] == 0x3c) || (mi->bits == 7 && s[i] == 0x5c)) && i < len - 1) {
      switch(j) {
        case 0:
          serial = i + 1; j++; break;
        case 1:
          class_name = i + 1; j++; break;
        case 2:
          dev_id = i + 1; j++; break;
        case 3:
          user_name = i + 1; j++; break;
        default:
          fprintf(stderr, "PnP-ID oops\n");
      }
    }
  }

  /* no end token... */

  return 0;
}


unsigned chk4id(ser_modem_t *mi)
{
  int i;

  if(!mi->buf_len) return 0;

  for(i = 0; i < mi->buf_len; i++) {
    if((mi->pnp = is_pnpinfo(mi, i))) break;
  }
  if(i == mi->buf_len) return 0;

  mi->garbage = i;

  return 1;
}

ser_modem_t *add_ser_modem_entry(ser_modem_t **sm, ser_modem_t *new_sm)
{
  while(*sm) sm = &(*sm)->next;
  return *sm = new_sm;
}

void dump_ser_modem_data(hd_data_t *hd_data)
{
  int j;
  ser_modem_t *sm;

  if(!(sm = hd_data->ser_modem)) return;

  ADD2LOG("----- serial modems -----\n");

  for(; sm; sm = sm->next) {
    ADD2LOG("%s\n", sm->dev_name);
    if(sm->serial) ADD2LOG("serial: \"%s\"\n", sm->serial);
    if(sm->class_name) ADD2LOG("class_name: \"%s\"\n", sm->class_name);
    if(sm->dev_id) ADD2LOG("dev_id: \"%s\"\n", sm->dev_id);
    if(sm->user_name) ADD2LOG("user_name: \"%s\"\n", sm->user_name);

    if(sm->garbage) {
      ADD2LOG("  pre_garbage[%u]: ", sm->garbage);
      hexdump(&hd_data->log, 1, sm->garbage, sm->buf);
      ADD2LOG("\n");  
    }

    if(sm->pnp) {
      ADD2LOG("  pnp[%u]: ", sm->pnp);
      hexdump(&hd_data->log, 1, sm->pnp, sm->buf + sm->garbage);
      ADD2LOG("\n");
    }

    if((j = sm->buf_len - (sm->garbage + sm->pnp))) {
      ADD2LOG("  post_garbage[%u]: ", j);
      hexdump(&hd_data->log, 1, j, sm->buf + sm->garbage + sm->pnp);
      ADD2LOG("\n");
    }

    if(sm->is_modem)
      ADD2LOG("  is modem\n");
    else
      ADD2LOG("  not a modem\n");

    if(sm->pnp) {
      ADD2LOG("  bits: %u\n", sm->bits);
      ADD2LOG("  PnP Rev: %u.%02u\n", sm->pnp_rev / 100, sm->pnp_rev % 100);
      ADD2LOG("  PnP ID: \"%s\"\n", sm->pnp_id);
    }

    if(sm->next) ADD2LOG("\n");
  }

  ADD2LOG("----- serial modems end -----\n");
}
