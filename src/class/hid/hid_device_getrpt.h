/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 TinyUSB contributors
 * SPDX-License-Identifier: MIT
 *
 * Deferred GET_REPORT, and SET_REPORT provenance, for the HID device class.
 * See the bottom of this file for why, and for how to re-apply after re-syncing
 * lib/tinyusb.
 *
 * This file is intentionally self-contained: everything lives here and in
 * hid_device_getrpt.c, so a future `git pull` of upstream cannot conflict with it.
 * The only edits to pre-existing TinyUSB sources are the `#if` blocks in
 * hid_device.c that route into hidd_getrpt_hold() / hidd_set_report_note_src().
 *
 * Fully MIT, safe to upstream as-is.
 */
#ifndef CLASS_HID_HID_DEVICE_GETRPT_H_
#define CLASS_HID_HID_DEVICE_GETRPT_H_

#include "tusb_option.h"

// Must be included from hid_device.c / hid_device.h *after* those, so that
// CFG_TUD_HID_EP_BUFSIZE and the tud_* prototypes are already visible.
//
// Default is 0, so an application that never heard of this compiles exactly as
// before (the whole feature below is then preprocessed away). Defining it in this
// new file rather than in tusb_option.h keeps that file free of our edits; it still
// lands after tusb_config.h, so an application can override it there.
#ifndef CFG_TUD_HID_DEFER_GET_REPORT
  #define CFG_TUD_HID_DEFER_GET_REPORT 0
#endif

#if (CFG_TUD_ENABLED && CFG_TUD_HID && CFG_TUD_HID_DEFER_GET_REPORT)

#include "common/tusb_types.h"  // tusb_control_request_t

#ifdef __cplusplus
 extern "C" {
#endif

//=====================================================================+
// Deferred GET_REPORT
//=====================================================================

// tud_hid_get_report_cb() runs synchronously inside usbd's
// process_setup_received(), after which the stack arms EP0's data stage. The
// callback must therefore already hold the bytes. That is fine for a device that
// owns its data, but not for a proxy that has to ask another machine and wait for a
// round trip. With this feature enabled, returning 0 parks the request instead of
// stalling it: the SETUP handler returns true without arming EP0, and the
// application completes it later with tud_hid_n_get_report_ready() or drops it with
// tud_hid_n_get_report_cancel().

//--------------------------------------------------------------------+
// Internal API, called from hid_device.c
//--------------------------------------------------------------------+

// Record a GET_REPORT SETUP without arming EP0, so the request waits for the
// application. Called immediately before tud_hid_get_report_cb(), which lets the
// callback inspect the request via tud_hid_n_get_report_pending_length().
// EP0 is single, so exactly one request can be parked at a time.
void hidd_getrpt_hold(uint8_t rhport, uint8_t instance, tusb_control_request_t const* request);

// True while a GET_REPORT request occupies the slot: already recorded during
// tud_hid_get_report_cb(), and afterwards still true if the callback returned 0.
// False also means the application completed or cancelled it from inside the
// callback, in which case EP0 is already taken care of.
bool hidd_getrpt_is_pending(void);

// Drop any parked request without touching EP0. Called from hidd_reset() on bus reset.
void hidd_getrpt_reset(void);

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

// The wLength the host asked for, or 0 when nothing is parked. Note that the
// `reqlen` handed to tud_hid_get_report_cb() has already had the report id byte
// removed when the host asked for a specific id, so a proxy that wants to forward the
// request verbatim to its own device needs this to recover the original length.
uint16_t tud_hid_n_get_report_pending_length(uint8_t instance);

// Complete the parked request. `report` is the complete data-stage content the host
// asked for, INCLUDING the leading report id byte when the device uses report ids --
// it is forwarded verbatim, so a proxy can hand back exactly what it read from another
// device without reproducing the report id prefixing rule. Over-length is clamped to
// the host's wLength and to the transfer buffer. Returns false if nothing is parked or
// the instance does not match; the request stays parked and can still be cancelled.
bool tud_hid_n_get_report_ready(uint8_t instance, void const* report, uint16_t len);

// Abandon the parked request, stalling EP0. Use this when the data never arrives,
// otherwise the host waits for its control transfer timeout.
//
// NOTE ON TIMING. tud_control_xfer() ignores its `request` argument and reads
// _usbd_dev.ctrl_xfer.request instead, so a reply sent after the host has already
// issued a new SETUP would be delivered against the *new* request. Cancel in bounded
// time -- well inside the host's own control-transfer timeout (5 s per USB 2.0 on
// Windows) -- rather than parking indefinitely. A new GET_REPORT SETUP re-parks the
// slot on its own (the slot is overwritten, the previous request is abandoned just as
// the host abandoned it), so a late reply is at worst wasted, not mis-delivered, unless
// the host moved on to an unrelated request.
bool tud_hid_n_get_report_cancel(uint8_t instance);

//=====================================================================+
// SET_REPORT provenance
//=====================================================================

// Where the report most recently delivered to tud_hid_set_report_cb() came from.
//
// Without this the callback cannot be forwarded faithfully, for two independent
// reasons:
//
//  1. TRANSPORT. The interrupt OUT endpoint and a control SET_REPORT both arrive at
//     the same callback. The OUT path hard-codes report_id = 0, and a control
//     SET_REPORT may legitimately carry report_id = 0 too, so
//     HidD_SetOutputReport(id=0) and WriteFile are indistinguishable by the callback
//     arguments. They are not interchangeable at the device: one is a control
//     transfer, the other an interrupt transfer, and a real device may act on only
//     one of them.
//
//  2. REPORT ID PREFIX. TinyUSB strips a leading report id from a control SET_REPORT
//     data stage before calling the callback, so `bufsize` alone cannot tell a host
//     that included the id from one that omitted it -- both yield the same callback
//     length. Vendor stacks do include it; captured from Logitech G HUB:
//
//       bmRequestType=0x21 bRequest=0x09 wValue=0x0210 wIndex=1 wLength=0x0007
//       data = 10 FF 00 1B 00 00 00
//
//     Forwarding the stripped form sends FF 00 1B 00 00 00 instead: a different
//     command, one byte short and without the id. HID++ devices ignore it silently --
//     no interrupt IN is pushed and no state changes, yet the control transfer still
//     acks, so nothing in the logs shows a failure.
//
// The two are one mutually exclusive, exhaustive classification, so they are exposed
// as a single enumeration rather than a pair of flags (which would admit the
// nonsensical "neither a control transfer nor known to carry an id" state).
typedef enum {
  TUD_HID_SET_REPORT_SRC_CONTROL_WITH_ID = 0, // control transfer, host put the report id in the data stage
  TUD_HID_SET_REPORT_SRC_CONTROL_NO_ID,       // control transfer, report id only in wValue
  TUD_HID_SET_REPORT_SRC_EP_OUT,              // interrupt OUT endpoint
} tud_hid_set_report_src_t;

//--------------------------------------------------------------------+
// Internal API, called from hid_device.c
//--------------------------------------------------------------------+

// Record which path invoked the callback. Call immediately before
// tud_hid_set_report_cb(), from both hidd_control_xfer_cb() and hidd_xfer_cb().
void hidd_set_report_note_src(tud_hid_set_report_src_t src);

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

// How the report most recently delivered to tud_hid_set_report_cb() arrived.
// Valid only inside tud_hid_set_report_cb(), and only meaningful when that callback
// is reached with report_type == HID_REPORT_TYPE_OUTPUT (Feature reports exist only
// on the control path).
tud_hid_set_report_src_t tud_hid_set_report_source(uint8_t instance);

#ifdef __cplusplus
 }
#endif

#endif // CFG_TUD_HID_DEFER_GET_REPORT

#endif // CLASS_HID_HID_DEVICE_GETRPT_H_