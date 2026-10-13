/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 TinyUSB contributors
 * SPDX-License-Identifier: MIT
 *
 * Deferred GET_REPORT and SET_REPORT provenance for the HID device class. See
 * hid_device_getrpt.h for what these are for and how to re-apply after re-syncing
 * lib/tinyusb.
 *
 * This file is intentionally self-contained: the only thing it needs from hid_device.c
 * is the EP0 control-transfer API, so the feature can live in one new file instead of
 * being threaded through the class implementation.
 */

#include "tusb_option.h"

#if (CFG_TUD_ENABLED && CFG_TUD_HID && CFG_TUD_HID_DEFER_GET_REPORT)

#include "device/usbd.h"
#include "device/usbd_pvt.h"

#include "hid_device.h"
#include "hid_device_getrpt.h"

// The class keeps its own ctrl buffer in _hidd_epbuf[] (hid_device.c), which is static
// there and not reachable from this file. Rather than exporting it, own a small buffer
// here: only one request can be parked at a time (EP0 is single), and once handed to
// tud_control_xfer() the data stage owns the buffer until the status stage completes,
// so a private buffer can never be handed out twice.
CFG_TUD_MEM_SECTION static struct {
  TUD_EPBUF_DEF(ctrl, CFG_TUD_HID_EP_BUFSIZE);
} _getrpt_epbuf;

// Keep our own copy of the request rather than reading it back out of
// _usbd_dev.ctrl_xfer: usbd does stash it there before invoking the class driver, but
// relying on that internal sequencing detail would make this feature hostage to
// unrelated usbd.c changes.
static struct {
  tusb_control_request_t request;
  uint8_t rhport;
  uint8_t instance;
} _getrpt;

static bool _getrpt_pending;

TU_ATTR_ALWAYS_INLINE static inline uint16_t getrpt_max_len(void) {
  return tu_min16(_getrpt.request.wLength, CFG_TUD_HID_EP_BUFSIZE);
}

//--------------------------------------------------------------------+
// Internal API, called from hid_device.c
//--------------------------------------------------------------------+

void hidd_getrpt_hold(uint8_t rhport, uint8_t instance, tusb_control_request_t const* request) {
  _getrpt.request = *request;
  _getrpt.rhport = rhport;
  _getrpt.instance = instance;
  _getrpt_pending = true;
  TU_LOG1("  HID DEFER GET_REPORT: itf=%u id=%u type=%u wLength=%u\r\n", instance, tu_u16_low(request->wValue),
          tu_u16_high(request->wValue), request->wLength);
}

bool hidd_getrpt_is_pending(void) {
  return _getrpt_pending;
}

void hidd_getrpt_reset(void) {
  // Bus reset killed the held request: the host is no longer waiting, and the slot must
  // not be answerable any more (EP0 has been re-armed underneath us).
  _getrpt_pending = false;
}

static tud_hid_set_report_src_t _set_report_src;

void hidd_set_report_note_src(tud_hid_set_report_src_t src) {
  _set_report_src = src;
}

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

uint16_t tud_hid_n_get_report_pending_length(uint8_t instance) {
  TU_VERIFY(_getrpt_pending && (_getrpt.instance == instance), 0);
  return getrpt_max_len();
}

bool tud_hid_n_get_report_ready(uint8_t instance, void const* report, uint16_t len) {
  TU_VERIFY(_getrpt_pending && (_getrpt.instance == instance));

  uint8_t* buf = _getrpt_epbuf.ctrl;
  uint16_t const max_len = getrpt_max_len();
  uint16_t const xferlen = tu_min16(len, max_len);
  TU_VERIFY(xferlen > 0);
  TU_VERIFY(0 == tu_memcpy_s(buf, max_len, report, xferlen));

  // Everything that can fail is checked, so commit now: EP0 belongs to this request and
  // the slot must not be reusable even if the arming itself fails.
  _getrpt_pending = false;

  return tud_control_xfer(_getrpt.rhport, &_getrpt.request, buf, xferlen);
}

bool tud_hid_n_get_report_cancel(uint8_t instance) {
  TU_VERIFY(_getrpt_pending && (_getrpt.instance == instance));
  _getrpt_pending = false;

  // Both directions, mirroring how usbd.c stalls a control request it cannot satisfy.
  usbd_edpt_stall(_getrpt.rhport, TU_EP0_IN);
  usbd_edpt_stall(_getrpt.rhport, TU_EP0_OUT);
  return true;
}

tud_hid_set_report_src_t tud_hid_set_report_source(uint8_t instance) {
  (void) instance;
  return _set_report_src;
}

#endif