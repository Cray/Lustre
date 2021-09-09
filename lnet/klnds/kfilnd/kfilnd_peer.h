/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kfilnd peer interface.
 * Copyright 2019 Cray Inc. All rights reserved.
 *
 */

#ifndef _KFILND_PEER_
#define _KFILND_PEER_

#include "kfilnd.h"

void kfilnd_peer_down(struct kfilnd_peer *peer);
void kfilnd_peer_put(struct kfilnd_peer *peer);
struct kfilnd_peer *kfilnd_peer_get(struct kfilnd_dev *dev, lnet_nid_t nid);
void kfilnd_peer_update_rx_contexts(struct kfilnd_peer *peer,
				    unsigned int rx_base,
				    unsigned int rx_count);
void kfilnd_peer_alive(struct kfilnd_peer *peer);
void kfilnd_peer_destroy(struct kfilnd_dev *dev);
void kfilnd_peer_init(struct kfilnd_dev *dev);
kfi_addr_t kfilnd_peer_get_kfi_addr(struct kfilnd_peer *peer);
u16 kfilnd_peer_target_rx_base(struct kfilnd_peer *peer);

#endif /* _KFILND_PEER_ */
