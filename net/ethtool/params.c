/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */

#include "netlink.h"

static const struct nla_policy get_params_policy[ETHTOOL_A_PARAMS_MAX + 1] = {
	[ETHTOOL_A_PARAMS_UNSPEC]		= { .type = NLA_REJECT },
	[ETHTOOL_A_PARAMS_DEV]			= { .type = NLA_NESTED },
	[ETHTOOL_A_PARAMS_INFOMASK]		= { .type = NLA_U32 },
	[ETHTOOL_A_PARAMS_COMPACT]		= { .type = NLA_FLAG },
	[ETHTOOL_A_PARAMS_COALESCE]		= { .type = NLA_REJECT },
};

struct params_data {
	struct common_req_info		reqinfo_base;

	/* everything below here will be reset for each device in dumps */
	struct common_reply_data	repdata_base;
	struct ethtool_coalesce		coalesce;
};

static int parse_params(struct common_req_info *req_info, struct sk_buff *skb,
			struct genl_info *info, const struct nlmsghdr *nlhdr)
{
	struct nlattr *tb[ETHTOOL_A_PARAMS_MAX + 1];
	int ret;

	ret = nlmsg_parse(nlhdr, GENL_HDRLEN, tb, ETHTOOL_A_PARAMS_MAX,
			  get_params_policy, info ? info->extack : NULL);
	if (ret < 0)
		return ret;

	if (tb[ETHTOOL_A_PARAMS_DEV]) {
		req_info->dev = ethnl_dev_get(info, tb[ETHTOOL_A_PARAMS_DEV]);
		if (IS_ERR(req_info->dev)) {
			ret = PTR_ERR(req_info->dev);
			req_info->dev = NULL;
			return ret;
		}
	}
	if (tb[ETHTOOL_A_PARAMS_INFOMASK])
		req_info->req_mask = nla_get_u32(tb[ETHTOOL_A_PARAMS_INFOMASK]);
	if (tb[ETHTOOL_A_PARAMS_COMPACT])
		req_info->compact = true;
	if (req_info->req_mask == 0)
		req_info->req_mask = ETHTOOL_IM_PARAMS_ALL;

	return 0;
}

static int ethnl_get_coalesce(struct net_device *dev,
			      struct ethtool_coalesce *data)
{
	if (!dev->ethtool_ops->get_coalesce)
		return -EOPNOTSUPP;
	return dev->ethtool_ops->get_coalesce(dev, data);
}

static int prepare_params(struct common_req_info *req_info,
			  struct genl_info *info)
{
	struct params_data *data =
		container_of(req_info, struct params_data, reqinfo_base);
	struct net_device *dev = data->repdata_base.dev;
	u32 req_mask = req_info->req_mask;
	int ret;

	ret = ethnl_before_ops(dev);
	if (ret < 0)
		return ret;
	if (req_mask & ETHTOOL_IM_PARAMS_COALESCE) {
		ret = ethnl_get_coalesce(dev, &data->coalesce);
		if (ret < 0)
			req_mask &= ~ETHTOOL_IM_PARAMS_COALESCE;
	}
	ethnl_after_ops(dev);

	data->repdata_base.info_mask = req_mask;
	if (req_info->req_mask & ~req_mask)
		warn_partial_info(info);
	return 0;
}

static int coalesce_size(void)
{
	return nla_total_size(20 * nla_total_size(sizeof(u32)) +
			      2 * nla_total_size(sizeof(u8)));
}

static int params_size(const struct common_req_info *req_info)
{
	struct params_data *data =
		container_of(req_info, struct params_data, reqinfo_base);
	u32 info_mask = data->repdata_base.info_mask;
	int len = 0;

	len += dev_ident_size();
	if (info_mask & ETHTOOL_IM_PARAMS_COALESCE)
		len += coalesce_size();

	return len;
}

static int fill_coalesce(struct sk_buff *skb, struct ethtool_coalesce *data)
{
	struct nlattr *nest;

	nest = nla_nest_start(skb, ETHTOOL_A_PARAMS_COALESCE);
	if (!nest)
		return -EMSGSIZE;
	if (nla_put_u32(skb, ETHTOOL_A_COALESCE_RX_USECS,
			data->rx_coalesce_usecs) ||
	    nla_put_u32(skb, ETHTOOL_A_COALESCE_RX_MAXFRM,
			data->rx_max_coalesced_frames) ||
	    nla_put_u32(skb, ETHTOOL_A_COALESCE_RX_USECS_IRQ,
			data->rx_coalesce_usecs_irq) ||
	    nla_put_u32(skb, ETHTOOL_A_COALESCE_RX_MAXFRM_IRQ,
			data->rx_max_coalesced_frames_irq) ||
	    nla_put_u32(skb, ETHTOOL_A_COALESCE_RX_USECS_LOW,
			data->rx_coalesce_usecs_low) ||
	    nla_put_u32(skb, ETHTOOL_A_COALESCE_RX_MAXFRM_LOW,
			data->rx_max_coalesced_frames_low) ||
	    nla_put_u32(skb, ETHTOOL_A_COALESCE_RX_USECS_HIGH,
			data->rx_coalesce_usecs_high) ||
	    nla_put_u32(skb, ETHTOOL_A_COALESCE_RX_MAXFRM_HIGH,
			data->rx_max_coalesced_frames_high) ||
	    nla_put_u32(skb, ETHTOOL_A_COALESCE_TX_USECS,
			data->tx_coalesce_usecs) ||
	    nla_put_u32(skb, ETHTOOL_A_COALESCE_TX_MAXFRM,
			data->tx_max_coalesced_frames) ||
	    nla_put_u32(skb, ETHTOOL_A_COALESCE_TX_USECS_IRQ,
			data->tx_coalesce_usecs_irq) ||
	    nla_put_u32(skb, ETHTOOL_A_COALESCE_TX_MAXFRM_IRQ,
			data->tx_max_coalesced_frames_irq) ||
	    nla_put_u32(skb, ETHTOOL_A_COALESCE_TX_USECS_LOW,
			data->tx_coalesce_usecs_low) ||
	    nla_put_u32(skb, ETHTOOL_A_COALESCE_TX_MAXFRM_LOW,
			data->tx_max_coalesced_frames_low) ||
	    nla_put_u32(skb, ETHTOOL_A_COALESCE_TX_USECS_HIGH,
			data->tx_coalesce_usecs_high) ||
	    nla_put_u32(skb, ETHTOOL_A_COALESCE_TX_MAXFRM_HIGH,
			data->tx_max_coalesced_frames_high) ||
	    nla_put_u32(skb, ETHTOOL_A_COALESCE_PKT_RATE_LOW,
			data->pkt_rate_low) ||
	    nla_put_u32(skb, ETHTOOL_A_COALESCE_PKT_RATE_HIGH,
			data->pkt_rate_high) ||
	    nla_put_u8(skb, ETHTOOL_A_COALESCE_RX_USE_ADAPTIVE,
		       !!data->use_adaptive_rx_coalesce) ||
	    nla_put_u8(skb, ETHTOOL_A_COALESCE_TX_USE_ADAPTIVE,
		       !!data->use_adaptive_tx_coalesce) ||
	    nla_put_u32(skb, ETHTOOL_A_COALESCE_RATE_SAMPLE_INTERVAL,
			data->rate_sample_interval) ||
	    nla_put_u32(skb, ETHTOOL_A_COALESCE_STATS_BLOCK_USECS,
			data->stats_block_coalesce_usecs)) {
		nla_nest_cancel(skb, nest);
		return -EMSGSIZE;
	}

	nla_nest_end(skb, nest);
	return 0;
}

static int fill_params(struct sk_buff *skb,
		       const struct common_req_info *req_info)
{
	struct params_data *data =
		container_of(req_info, struct params_data, reqinfo_base);
	u32 info_mask = data->repdata_base.info_mask;
	int ret;

	if (info_mask & ETHTOOL_IM_PARAMS_COALESCE) {
		ret = fill_coalesce(skb, &data->coalesce);
		if (ret < 0)
			return ret;
	}

	return 0;
}

const struct get_request_ops params_request_ops = {
	.request_cmd		= ETHNL_CMD_GET_PARAMS,
	.reply_cmd		= ETHNL_CMD_SET_PARAMS,
	.dev_attrtype		= ETHTOOL_A_PARAMS_DEV,
	.data_size		= sizeof(struct params_data),
	.repdata_offset		= offsetof(struct params_data, repdata_base),

	.parse_request		= parse_params,
	.prepare_data		= prepare_params,
	.reply_size		= params_size,
	.fill_reply		= fill_params,
};

/* SET_PARAMS */

static const struct nla_policy set_params_policy[ETHTOOL_A_PARAMS_MAX + 1] = {
	[ETHTOOL_A_PARAMS_UNSPEC]		= { .type = NLA_REJECT },
	[ETHTOOL_A_PARAMS_DEV]			= { .type = NLA_NESTED },
	[ETHTOOL_A_PARAMS_INFOMASK]		= { .type = NLA_REJECT },
	[ETHTOOL_A_PARAMS_COMPACT]		= { .type = NLA_FLAG },
	[ETHTOOL_A_PARAMS_COALESCE]		= { .type = NLA_NESTED },
};

static const struct nla_policy coalesce_policy[ETHTOOL_A_COALESCE_MAX + 1] = {
	[ETHTOOL_A_COALESCE_UNSPEC]		= { .type = NLA_REJECT },
	[ETHTOOL_A_COALESCE_RX_USECS]		= { .type = NLA_U32 },
	[ETHTOOL_A_COALESCE_RX_MAXFRM]		= { .type = NLA_U32 },
	[ETHTOOL_A_COALESCE_RX_USECS_IRQ]	= { .type = NLA_U32 },
	[ETHTOOL_A_COALESCE_RX_MAXFRM_IRQ]	= { .type = NLA_U32 },
	[ETHTOOL_A_COALESCE_RX_USECS_LOW]	= { .type = NLA_U32 },
	[ETHTOOL_A_COALESCE_RX_MAXFRM_LOW]	= { .type = NLA_U32 },
	[ETHTOOL_A_COALESCE_RX_USECS_HIGH]	= { .type = NLA_U32 },
	[ETHTOOL_A_COALESCE_RX_MAXFRM_HIGH]	= { .type = NLA_U32 },
	[ETHTOOL_A_COALESCE_TX_USECS]		= { .type = NLA_U32 },
	[ETHTOOL_A_COALESCE_TX_MAXFRM]		= { .type = NLA_U32 },
	[ETHTOOL_A_COALESCE_TX_USECS_IRQ]	= { .type = NLA_U32 },
	[ETHTOOL_A_COALESCE_TX_MAXFRM_IRQ]	= { .type = NLA_U32 },
	[ETHTOOL_A_COALESCE_TX_USECS_LOW]	= { .type = NLA_U32 },
	[ETHTOOL_A_COALESCE_TX_MAXFRM_LOW]	= { .type = NLA_U32 },
	[ETHTOOL_A_COALESCE_TX_USECS_HIGH]	= { .type = NLA_U32 },
	[ETHTOOL_A_COALESCE_TX_MAXFRM_HIGH]	= { .type = NLA_U32 },
	[ETHTOOL_A_COALESCE_PKT_RATE_LOW]	= { .type = NLA_U32 },
	[ETHTOOL_A_COALESCE_PKT_RATE_HIGH]	= { .type = NLA_U32 },
	[ETHTOOL_A_COALESCE_RX_USE_ADAPTIVE]	= { .type = NLA_U8 },
	[ETHTOOL_A_COALESCE_TX_USE_ADAPTIVE]	= { .type = NLA_U8 },
	[ETHTOOL_A_COALESCE_RATE_SAMPLE_INTERVAL] = { .type = NLA_U32 },
	[ETHTOOL_A_COALESCE_STATS_BLOCK_USECS]	= { .type = NLA_U32 },
};

static int update_coalesce(struct genl_info *info, struct net_device *dev,
			   struct nlattr *nest)
{
	struct nlattr *tb[ETHTOOL_A_COALESCE_MAX + 1];
	struct ethtool_coalesce data = {};
	bool mod = false;
	int ret;

	if (!nest)
		return 0;
	if (!dev->ethtool_ops->get_coalesce || !dev->ethtool_ops->set_coalesce)
		return -EOPNOTSUPP;
	ret = dev->ethtool_ops->get_coalesce(dev, &data);
	if (ret < 0)
		return ret;

	ret = nla_parse_nested(tb, ETHTOOL_A_COALESCE_MAX, nest,
			       coalesce_policy, info->extack);
	if (ret < 0)
		return ret;

	if (ethnl_update_u32(&data.rx_coalesce_usecs,
			     tb[ETHTOOL_A_COALESCE_RX_USECS]))
		mod = true;
	if (ethnl_update_u32(&data.rx_max_coalesced_frames,
			     tb[ETHTOOL_A_COALESCE_RX_MAXFRM]))
		mod = true;
	if (ethnl_update_u32(&data.rx_coalesce_usecs_irq,
			     tb[ETHTOOL_A_COALESCE_RX_USECS_IRQ]))
		mod = true;
	if (ethnl_update_u32(&data.rx_max_coalesced_frames_irq,
			     tb[ETHTOOL_A_COALESCE_RX_MAXFRM_IRQ]))
		mod = true;
	if (ethnl_update_u32(&data.rx_coalesce_usecs_low,
			     tb[ETHTOOL_A_COALESCE_RX_USECS_LOW]))
		mod = true;
	if (ethnl_update_u32(&data.rx_max_coalesced_frames_low,
			     tb[ETHTOOL_A_COALESCE_RX_MAXFRM_LOW]))
		mod = true;
	if (ethnl_update_u32(&data.rx_coalesce_usecs_high,
			     tb[ETHTOOL_A_COALESCE_RX_USECS_HIGH]))
		mod = true;
	if (ethnl_update_u32(&data.rx_max_coalesced_frames_high,
			     tb[ETHTOOL_A_COALESCE_RX_MAXFRM_HIGH]))
		mod = true;
	if (ethnl_update_u32(&data.tx_coalesce_usecs,
			     tb[ETHTOOL_A_COALESCE_TX_USECS]))
		mod = true;
	if (ethnl_update_u32(&data.tx_max_coalesced_frames,
			     tb[ETHTOOL_A_COALESCE_TX_MAXFRM]))
		mod = true;
	if (ethnl_update_u32(&data.tx_coalesce_usecs_irq,
			     tb[ETHTOOL_A_COALESCE_TX_USECS_IRQ]))
		mod = true;
	if (ethnl_update_u32(&data.tx_max_coalesced_frames_irq,
			     tb[ETHTOOL_A_COALESCE_TX_MAXFRM_IRQ]))
		mod = true;
	if (ethnl_update_u32(&data.tx_coalesce_usecs_low,
			     tb[ETHTOOL_A_COALESCE_TX_USECS_LOW]))
		mod = true;
	if (ethnl_update_u32(&data.tx_max_coalesced_frames_low,
			     tb[ETHTOOL_A_COALESCE_TX_MAXFRM_LOW]))
		mod = true;
	if (ethnl_update_u32(&data.tx_coalesce_usecs_high,
			     tb[ETHTOOL_A_COALESCE_TX_USECS_HIGH]))
		mod = true;
	if (ethnl_update_u32(&data.tx_max_coalesced_frames_high,
			     tb[ETHTOOL_A_COALESCE_TX_MAXFRM_HIGH]))
		mod = true;
	if (ethnl_update_u32(&data.pkt_rate_low,
			     tb[ETHTOOL_A_COALESCE_PKT_RATE_LOW]))
		mod = true;
	if (ethnl_update_u32(&data.pkt_rate_high,
			     tb[ETHTOOL_A_COALESCE_PKT_RATE_HIGH]))
		mod = true;
	if (ethnl_update_bool32(&data.use_adaptive_rx_coalesce,
				tb[ETHTOOL_A_COALESCE_RX_USE_ADAPTIVE]))
		mod = true;
	if (ethnl_update_bool32(&data.use_adaptive_tx_coalesce,
				tb[ETHTOOL_A_COALESCE_TX_USE_ADAPTIVE]))
		mod = true;
	if (ethnl_update_u32(&data.rate_sample_interval,
			     tb[ETHTOOL_A_COALESCE_RATE_SAMPLE_INTERVAL]))
		mod = true;
	if (ethnl_update_u32(&data.stats_block_coalesce_usecs,
			     tb[ETHTOOL_A_COALESCE_STATS_BLOCK_USECS]))
		mod = true;

	if (!mod)
		return 0;
	ret = dev->ethtool_ops->set_coalesce(dev, &data);
	return (ret < 0) ? ret : 1;
}

int ethnl_set_params(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *tb[ETHTOOL_A_PARAMS_MAX + 1];
	struct net_device *dev;
	u32 req_mask = 0;
	int ret;

	ret = nlmsg_parse(info->nlhdr, GENL_HDRLEN, tb, ETHTOOL_A_PARAMS_MAX,
			  set_params_policy, info->extack);
	if (ret < 0)
		return ret;
	dev = ethnl_dev_get(info, tb[ETHTOOL_A_PARAMS_DEV]);
	if (IS_ERR(dev))
		return PTR_ERR(dev);

	rtnl_lock();
	ret = ethnl_before_ops(dev);
	if (ret < 0)
		goto out_rtnl;
	ret = update_coalesce(info, dev, tb[ETHTOOL_A_PARAMS_COALESCE]);
	if (ret < 0)
		goto out_ops;
	if (ret)
		req_mask |= ETHTOOL_IM_PARAMS_COALESCE;

	ret = 0;
out_ops:
	if (req_mask)
		ethtool_notify(dev, NULL, ETHNL_CMD_SET_PARAMS, req_mask, NULL);
	ethnl_after_ops(dev);
out_rtnl:
	rtnl_unlock();
	dev_put(dev);
	return ret;
}
