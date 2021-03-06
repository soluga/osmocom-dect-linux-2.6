/*
 * DECT DLC Layer
 *
 * Copyright (c) 2009 Patrick McHardy <kaber@trash.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifdef CONFIG_DECT_DEBUG
#define DEBUG
#endif

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/skbuff.h>
#include <linux/net.h>
#include <linux/dect.h>
#include <net/dect/dect.h>

#define mc_debug(mc, fmt, args...) \
	pr_debug("MC (MCEI %u/%s): " fmt, \
		 (mc)->mcei, dect_mc_states[(mc)->state], ## args)

static const char * const dect_mc_states[] = {
	[DECT_MAC_CONN_CLOSED]		= "CLOSED",
	[DECT_MAC_CONN_OPEN_PENDING]	= "OPEN_PENDING",
	[DECT_MAC_CONN_OPEN]		= "OPEN",
};

static struct dect_mac_conn *
dect_mac_conn_get_by_mcei(const struct dect_cluster *cl, u32 mcei)
{
	struct dect_mac_conn *mc;

	list_for_each_entry(mc, &cl->mac_connections, list) {
		if (mc->mcei == mcei)
			return mc;
	}
	return NULL;
}

struct dect_mac_conn *
dect_mac_conn_get_by_mci(const struct dect_cluster *cl, const struct dect_mci *mci)
{
	struct dect_mac_conn *mc;

	list_for_each_entry(mc, &cl->mac_connections, list) {
		if (!dect_ari_cmp(&mc->mci.ari, &mci->ari) &&
		    !dect_pmid_cmp(&mc->mci.pmid, &mci->pmid) &&
		    mc->mci.lcn == mci->lcn)
			return mc;
	}
	return NULL;
}
EXPORT_SYMBOL_GPL(dect_mac_conn_get_by_mci);

void dect_dlc_mac_conn_destroy(struct dect_mac_conn *mc)
{
	mc_debug(mc, "destroy\n");
	list_del(&mc->list);
	kfree(mc);
}

void dect_dlc_mac_conn_bind(struct dect_mac_conn *mc)
{
	mc_debug(mc, "bind use %u\n", mc->use);
	mc->use++;
}
EXPORT_SYMBOL_GPL(dect_dlc_mac_conn_bind);

void dect_dlc_mac_conn_unbind(struct dect_mac_conn *mc)
{
	mc_debug(mc, "unbind use %u\n", mc->use);
	if (--mc->use)
		return;

	if (mc->state == DECT_MAC_CONN_OPEN ||
	    mc->state == DECT_MAC_CONN_OPEN_PENDING)
		dect_mac_dis_req(mc->cl, mc->mcei);

	dect_dlc_mac_conn_destroy(mc);
}
EXPORT_SYMBOL_GPL(dect_dlc_mac_conn_unbind);

struct dect_mac_conn *dect_mac_conn_init(struct dect_cluster *cl,
					 const struct dect_mci *mci,
					 const struct dect_mbc_id *id)
{
	struct dect_mac_conn *mc;

	mc = kzalloc(sizeof(*mc), GFP_ATOMIC);
	if (mc == NULL)
		return NULL;

	mc->cl    = cl;
	mc->mcei  = id != NULL ? id->mcei : dect_mbc_alloc_mcei(cl);
	memcpy(&mc->mci, mci, sizeof(mc->mci));
	mc->state = DECT_MAC_CONN_CLOSED;
	mc_debug(mc, "init\n");

	list_add_tail(&mc->list, &cl->mac_connections);
	return mc;
}

static void dect_mac_conn_state_change(struct dect_mac_conn *mc,
				       enum dect_mac_conn_states state)
{
	mc_debug(mc, "state change: %s (%u) -> %s (%u)\n",
		 dect_mc_states[mc->state], mc->state,
		 dect_mc_states[state], state);

	mc->state = state;
	dect_cplane_notify_state_change(mc);
}

int dect_dlc_mac_conn_establish(struct dect_mac_conn *mc,
				const struct dect_mac_conn_params *mcp)
{
	struct dect_mbc_id mid = {
		.mcei		= mc->mcei,
		.ari		= mc->mci.ari,
		.pmid		= mc->mci.pmid,
		.ecn		= mc->mci.lcn,
	};
	int err;

	err = dect_mac_con_req(mc->cl, &mid, mcp);
	if (err < 0)
		return err;
	dect_mac_conn_state_change(mc, DECT_MAC_CONN_OPEN_PENDING);
	return 0;
}

int dect_mac_con_cfm(struct dect_cluster *cl, u32 mcei,
		     const struct dect_mac_conn_params *mcp)
{
	struct dect_mac_conn *mc;

	mc = dect_mac_conn_get_by_mcei(cl, mcei);
	if (WARN_ON(mc == NULL))
		return -ENOENT;
	mc->mcp = *mcp;

	mc_debug(mc, "MAC_CON-cfm\n");
	dect_mac_conn_state_change(mc, DECT_MAC_CONN_OPEN);
	return 0;
}

int dect_mac_con_ind(struct dect_cluster *cl, const struct dect_mbc_id *id,
		     const struct dect_mac_conn_params *mcp)
{
	struct dect_mac_conn *mc;
	struct dect_mci mci = {
		.ari		= id->ari,
		.pmid		= id->pmid,
		.lcn		= id->ecn & DECT_LCN_MASK,
	};

	mc = dect_mac_conn_init(cl, &mci, id);
	if (mc == NULL)
		return -ENOMEM;
	mc->mcp = *mcp;

	mc_debug(mc, "MAC_CON-ind\n");
	dect_mac_conn_state_change(mc, DECT_MAC_CONN_OPEN);
	return 0;
}

int dect_dlc_mac_conn_enc_key_req(struct dect_mac_conn *mc, u64 ck)
{
	mc->ck = ck;
	return dect_mac_enc_key_req(mc->cl, mc->mcei, ck);
}

int dect_dlc_mac_conn_enc_eks_req(struct dect_mac_conn *mc,
				  enum dect_cipher_states status)
{
	return dect_mac_enc_eks_req(mc->cl, mc->mcei, status);
}

/* Encryption status change confirmation from CCF */
void dect_mac_enc_eks_cfm(struct dect_cluster *cl, u32 mcei,
			  enum dect_cipher_states status)

{
	struct dect_mac_conn *mc;

	mc = dect_mac_conn_get_by_mcei(cl, mcei);
	if (WARN_ON(mc == NULL))
		return;
	//dect_cplane_mac_enc_eks_ind(mc, status);
}

/* Encryption status change indication from CCF */
void dect_mac_enc_eks_ind(struct dect_cluster *cl, u32 mcei,
			  enum dect_cipher_states status)

{
	struct dect_mac_conn *mc;

	mc = dect_mac_conn_get_by_mcei(cl, mcei);
	if (WARN_ON(mc == NULL))
		return;
	mc_debug(mc, "MAC_ENC_EKS-ind: status: %u\n", status);
	dect_cplane_mac_enc_eks_ind(mc, status);
}

/* Disconnection indication from CCF */
int dect_mac_dis_ind(struct dect_cluster *cl, u32 mcei,
		     enum dect_release_reasons reason)
{
	struct dect_mac_conn *mc;

	mc = dect_mac_conn_get_by_mcei(cl, mcei);
	if (WARN_ON(mc == NULL))
		return -ENOENT;

	mc_debug(mc, "MAC_DIS-ind: reason: %x\n", reason);
	dect_mac_conn_state_change(mc, DECT_MAC_CONN_CLOSED);
	/* If nothing is using the connection, release immediately */
	if (mc->use == 0)
		dect_dlc_mac_conn_destroy(mc);
	else
		dect_cplane_mac_dis_ind(mc, reason);
	return 0;
}

/* Data indication from CCF */
void dect_mac_co_data_ind(struct dect_cluster *cl, u32 mcei,
			  enum dect_data_channels chan,
			  struct sk_buff *skb)
{
	struct dect_mac_conn *mc;

	mc = dect_mac_conn_get_by_mcei(cl, mcei);
	if (WARN_ON(mc == NULL))
		goto err;

	mc_debug(mc, "MAC_CO_DATA-ind: chan: %u len: %u\n", chan, skb->len);
	switch (chan) {
	case DECT_MC_C_S:
	case DECT_MC_C_F:
		return dect_cplane_rcv(mc, chan, skb);
	case DECT_MC_I_N:
	case DECT_MC_I_P:
		return dect_uplane_rcv(mc, chan, skb);
	default:
		goto err;
	}
err:
	kfree_skb(skb);
}

/* Data-ready indication from CCF */
struct sk_buff *dect_mac_co_dtr_ind(struct dect_cluster *cl, u32 mcei,
				    enum dect_data_channels chan)
{
	struct dect_mac_conn *mc;

	mc = dect_mac_conn_get_by_mcei(cl, mcei);
	if (mc == NULL) {
		if (net_ratelimit())
			pr_debug("DLC: DTR no connection\n");
		return NULL;
	}

	mc_debug(mc, "MAC_CO_DTR-ind: chan: %u\n", chan);
	switch (chan) {
	case DECT_MC_C_S:
	case DECT_MC_C_F:
		return dect_cplane_dtr(mc, chan);
	case DECT_MC_I_N:
	case DECT_MC_I_P:
		return dect_uplane_dtr(mc, chan);
	default:
		return NULL;
	}
}
