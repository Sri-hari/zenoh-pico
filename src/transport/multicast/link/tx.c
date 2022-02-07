/*
 * Copyright (c) 2017, 2021 ADLINK Technology Inc.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Apache License, Version 2.0
 * which is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
 *
 * Contributors:
 *   ADLINK zenoh team, <zenoh@adlink-labs.tech>
 */

#include "zenoh-pico/protocol/msgcodec.h"
#include "zenoh-pico/transport/link/tx.h"
#include "zenoh-pico/transport/utils.h"
#include "zenoh-pico/utils/logging.h"

/*------------------ SN helper ------------------*/
z_zint_t __zn_multicast_get_sn(_zn_transport_multicast_t *ztm, zn_reliability_t reliability)
{
    z_zint_t sn;
    // Get the sequence number and update it in modulo operation
    if (reliability == zn_reliability_t_RELIABLE)
    {
        sn = ztm->sn_tx_reliable;
        ztm->sn_tx_reliable = _zn_sn_increment(ztm->sn_resolution, ztm->sn_tx_reliable);
    }
    else
    {
        sn = ztm->sn_tx_best_effort;
        ztm->sn_tx_best_effort = _zn_sn_increment(ztm->sn_resolution, ztm->sn_tx_best_effort);
    }
    return sn;
}

void __zn_multicast_rollback_sn(_zn_transport_multicast_t *ztm, zn_reliability_t reliability)
{
    if (reliability == zn_reliability_t_RELIABLE)
        ztm->sn_tx_reliable = _zn_sn_decrement(ztm->sn_resolution, ztm->sn_tx_reliable);
    else
        ztm->sn_tx_best_effort = _zn_sn_decrement(ztm->sn_resolution, ztm->sn_tx_best_effort);
}

int _zn_multicast_send_t_msg(_zn_transport_multicast_t *ztm, const _zn_transport_message_t *t_msg)
{
    _Z_DEBUG(">> send session message\n");

    // Acquire the lock
    z_mutex_lock(&ztm->mutex_tx);

    // Prepare the buffer eventually reserving space for the message length
    __unsafe_zn_prepare_wbuf(&ztm->wbuf, ztm->link->is_streamed);

    // Encode the session message
    int res = _zn_transport_message_encode(&ztm->wbuf, t_msg);
    if (res == 0)
    {
        // Write the message legnth in the reserved space if needed
        __unsafe_zn_finalize_wbuf(&ztm->wbuf, ztm->link->is_streamed);
        // Send the wbuf on the socket
        res = _zn_link_send_wbuf(ztm->link, &ztm->wbuf);
        // Mark the session that we have transmitted data
        ztm->transmitted = 1;
    }
    else
    {
        _Z_DEBUG("Dropping session message because it is too large");
    }

    // Release the lock
    z_mutex_unlock(&ztm->mutex_tx);

    return res;
}

int _zn_multicast_send_z_msg(zn_session_t *zn, _zn_zenoh_message_t *z_msg, zn_reliability_t reliability, zn_congestion_control_t cong_ctrl)
{
    _Z_DEBUG(">> send zenoh message\n");

    int res = 0;
    _zn_transport_multicast_t *ztm = &zn->tp->transport.multicast;

    // Acquire the lock and drop the message if needed
    if (cong_ctrl == zn_congestion_control_t_BLOCK)
    {
        z_mutex_lock(&ztm->mutex_tx);
    }
    else
    {
        int locked = z_mutex_trylock(&ztm->mutex_tx);
        if (locked != 0)
            goto ERR_1;
    }

    // Prepare the buffer eventually reserving space for the message length
    __unsafe_zn_prepare_wbuf(&ztm->wbuf, ztm->link->is_streamed);

    // Get the next sequence number
    z_zint_t sn = __zn_multicast_get_sn(ztm, reliability);

    // Create and encode the frame header that carries the zenoh message
    _zn_transport_message_t t_msg = _zn_t_msg_make_frame(reliability, sn, _zn_zenoh_message_vec_make(0));
    res = _zn_transport_message_encode(&ztm->wbuf, &t_msg);
    if (res != 0)
        goto ERR_2;

    // Encode the zenoh message
    res = _zn_zenoh_message_encode(&ztm->wbuf, z_msg);
    if (res == 0)
    {
        // Write the message legnth in the reserved space if needed
        __unsafe_zn_finalize_wbuf(&ztm->wbuf, ztm->link->is_streamed);

        // Send the wbuf on the socket
        res = _zn_link_send_wbuf(ztm->link, &ztm->wbuf);
        if (res != 0)
            goto ERR_2;
    }
    else
    {
        // Fragment as it does not fit in the current batch
        // Create an expandable wbuf for fragmentation
        _z_wbuf_t fbf = _z_wbuf_make(ZN_FRAG_BUF_TX_CHUNK, 1);

        // Encode the message on the expandable wbuf
        res = _zn_zenoh_message_encode(&fbf, z_msg);
        if (res != 0)
        {
            _Z_DEBUG("Dropping zenoh message because it can not be fragmented");
            goto ERR_2;
        }

        // Fragment and send the message
        do
        {
            // Clear the buffer for serialization
            __unsafe_zn_prepare_wbuf(&ztm->wbuf, ztm->link->is_streamed);

            // Serialize one fragment
            res = _zn_serialize_zenoh_fragment(&ztm->wbuf, &fbf, reliability, sn);
            if (res != 0)
                goto ERR_2;

            // Write the message legnth in the reserved space if needed
            __unsafe_zn_finalize_wbuf(&ztm->wbuf, ztm->link->is_streamed);

            // Send the wbuf on the socket
            res = _zn_link_send_wbuf(ztm->link, &ztm->wbuf);
            if (res != 0)
                goto ERR_2;

            sn = __zn_multicast_get_sn(ztm, reliability);
        } while (_z_wbuf_len(&fbf) > 0);

        // Rollback the last SN increment that is not used
        __zn_multicast_rollback_sn(ztm, reliability);
    }

    ztm->transmitted = 1;

    z_mutex_unlock(&ztm->mutex_tx);

    return res;

ERR_2:
    // Rollback already acquired SN and not used
    __zn_multicast_rollback_sn(ztm, reliability);
    z_mutex_unlock(&ztm->mutex_tx);

ERR_1:
    return res;
}