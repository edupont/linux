// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

#include <net/sock.h>
#include <linux/ethtool_netlink.h>
#include "netlink.h"

static struct genl_family ethtool_genl_family;

static bool ethnl_ok __read_mostly;
static u32 ethnl_bcast_seq;

static const struct nla_policy dev_policy[ETHTOOL_A_DEV_MAX + 1] = {
	[ETHTOOL_A_DEV_UNSPEC]	= { .type = NLA_REJECT },
	[ETHTOOL_A_DEV_INDEX]	= { .type = NLA_U32 },
	[ETHTOOL_A_DEV_NAME]	= { .type = NLA_NUL_STRING,
				    .len = IFNAMSIZ - 1 },
};

/**
 * ethnl_dev_get() - get device identified by nested attribute
 * @info: genetlink info (also used for extack error reporting)
 * @nest: nest attribute with device identification
 *
 * Finds the network device identified by ETHTOOL_A_DEV_INDEX (ifindex) or
 * ETHTOOL_A_DEV_NAME (name) attributes in a nested attribute @nest. If both
 * are supplied, they must identify the same device. If successful, takes
 * a reference to the device which is to be released by caller.
 *
 * Return: pointer to the device if successful, ERR_PTR(err) on error
 */
struct net_device *ethnl_dev_get(struct genl_info *info, struct nlattr *nest)
{
	struct net *net = genl_info_net(info);
	struct nlattr *tb[ETHTOOL_A_DEV_MAX + 1];
	struct net_device *dev;
	int ret;

	if (!nest) {
		GENL_SET_ERR_MSG(info, "device identification missing");
		return ERR_PTR(-EINVAL);
	}
	ret = nla_parse_nested(tb, ETHTOOL_A_DEV_MAX, nest, dev_policy,
			       info->extack);
	if (ret < 0)
		return ERR_PTR(ret);

	if (tb[ETHTOOL_A_DEV_INDEX]) {
		dev = dev_get_by_index(net,
				       nla_get_u32(tb[ETHTOOL_A_DEV_INDEX]));
		if (!dev) {
			NL_SET_ERR_MSG_ATTR(info->extack,
					    tb[ETHTOOL_A_DEV_INDEX],
					    "no device matches ifindex");
			return ERR_PTR(-ENODEV);
		}
		/* if both ifindex and ifname are passed, they must match */
		if (tb[ETHTOOL_A_DEV_NAME]) {
			const char *nl_name = nla_data(tb[ETHTOOL_A_DEV_NAME]);

			if (strncmp(dev->name, nl_name, IFNAMSIZ)) {
				dev_put(dev);
				NL_SET_ERR_MSG_ATTR(info->extack, nest,
						    "ifindex and name do not match");
				return ERR_PTR(-ENODEV);
			}
		}
		return dev;
	} else if (tb[ETHTOOL_A_DEV_NAME]) {
		dev = dev_get_by_name(net, nla_data(tb[ETHTOOL_A_DEV_NAME]));
		if (!dev) {
			NL_SET_ERR_MSG_ATTR(info->extack,
					    tb[ETHTOOL_A_DEV_NAME],
					    "no device matches name");
			return ERR_PTR(-ENODEV);
		}
	} else {
		NL_SET_ERR_MSG_ATTR(info->extack, nest,
				    "neither ifindex nor name specified");
		return ERR_PTR(-EINVAL);
	}

	if (!netif_device_present(dev)) {
		dev_put(dev);
		GENL_SET_ERR_MSG(info, "device not present");
		return ERR_PTR(-ENODEV);
	}

	return dev;
}

/**
 * ethnl_fill_dev() - Put device identification nest into a message
 * @msg:      skb with the message
 * @dev:      network device to describe
 * @attrtype: attribute type to use for the nest
 *
 * Create a nested attribute with attributes describing given network device.
 * Clean up on error.
 *
 * Return: 0 on success, error value (-EMSGSIZE only) on error
 */
int ethnl_fill_dev(struct sk_buff *msg, struct net_device *dev, u16 attrtype)
{
	struct nlattr *nest;
	int ret = -EMSGSIZE;

	nest = nla_nest_start(msg, attrtype);
	if (!nest)
		return -EMSGSIZE;

	if (nla_put_u32(msg, ETHTOOL_A_DEV_INDEX, (u32)dev->ifindex))
		goto err;
	if (nla_put_string(msg, ETHTOOL_A_DEV_NAME, dev->name))
		goto err;

	nla_nest_end(msg, nest);
	return 0;
err:
	nla_nest_cancel(msg, nest);
	return ret;
}

/**
 * ethnl_reply_init() - Create skb for a reply and fill device identification
 * @payload: payload length (without netlink and genetlink header)
 * @dev:     device the reply is about (may be null)
 * @cmd:     ETHNL_CMD_* command for reply
 * @info:    genetlink info of the received packet we respond to
 * @ehdrp:   place to store payload pointer returned by genlmsg_new()
 *
 * Return: pointer to allocated skb on success, NULL on error
 */
struct sk_buff *ethnl_reply_init(size_t payload, struct net_device *dev, u8 cmd,
				 u16 dev_attrtype, struct genl_info *info,
				 void **ehdrp)
{
	struct sk_buff *skb;
	void *ehdr;

	skb = genlmsg_new(payload, GFP_KERNEL);
	if (!skb)
		goto err_alloc;

	ehdr = genlmsg_put_reply(skb, info, &ethtool_genl_family, 0, cmd);
	if (!ehdr)
		goto err;
	if (ehdrp)
		*ehdrp = ehdr;
	if (dev) {
		int ret = ethnl_fill_dev(skb, dev, dev_attrtype);

		if (ret < 0)
			goto err;
	}
	return skb;

err_alloc:
	nlmsg_free(skb);
err:
	if (info)
		GENL_SET_ERR_MSG(info, "failed to allocate reply message");
	return NULL;
}

static void *ethnl_bcastmsg_put(struct sk_buff *skb, u8 cmd)
{
	return genlmsg_put(skb, 0, ++ethnl_bcast_seq, &ethtool_genl_family, 0,
			   cmd);
}

static int ethnl_multicast(struct sk_buff *skb, struct net_device *dev)
{
	return genlmsg_multicast_netns(&ethtool_genl_family, dev_net(dev), skb,
				       0, ETHNL_MCGRP_MONITOR, GFP_KERNEL);
}

/* GET request helpers */

const struct get_request_ops *get_requests[__ETHNL_CMD_CNT] = {
	[ETHNL_CMD_GET_STRSET]		= &strset_request_ops,
};

/**
 * ethnl_alloc_get_data() - Allocate and initialize data for a GET request
 * @ops: instance of struct get_request_ops describing size and layout
 *
 * This initializes only the first part (req_info), second part (reply_data)
 * is initialized before filling the reply data into it (which is done for
 * each iteration in dump requests).
 *
 * Return: pointer to allocated and initialized data, NULL on error
 */
static struct common_req_info *
ethnl_alloc_get_data(const struct get_request_ops *ops)
{
	struct common_req_info *req_info;

	req_info = kmalloc(ops->data_size, GFP_KERNEL);
	if (!req_info)
		return NULL;

	memset(req_info, '\0', ops->repdata_offset);
	req_info->reply_data =
		(struct common_reply_data *)((char *)req_info +
					     ops->repdata_offset);

	return req_info;
}

/**
 * ethnl_free_get_data() - free GET request data
 * @ops: instance of struct get_request_ops describing the layout
 * @req_info: pointer to embedded struct common_req_info (at offset 0)
 *
 * Calls ->cleanup() handler if defined and frees the data block.
 */
static void ethnl_free_get_data(const struct get_request_ops *ops,
				struct common_req_info *req_info)
{
	if (ops->cleanup)
		ops->cleanup(req_info);
	kfree(req_info);
}

/**
 * ethnl_init_reply_data() - Initialize reply data for GET request
 * @req_info: pointer to embedded struct common_req_info
 * @ops:      instance of struct get_request_ops describing the layout
 * @dev:      network device to initialize the reply for
 *
 * Fills the reply data part with zeros and sets the dev member. Must be called
 * before calling the ->fill_reply() callback (for each iteration when handling
 * dump requests).
 */
static void ethnl_init_reply_data(const struct common_req_info *req_info,
				  const struct get_request_ops *ops,
				  struct net_device *dev)
{
	memset(req_info->reply_data, '\0',
	       ops->data_size - ops->repdata_offset);
	req_info->reply_data->dev = dev;
}

/* generic ->doit() handler for GET type requests */
int ethnl_get_doit(struct sk_buff *skb, struct genl_info *info)
{
	const u8 cmd = info->genlhdr->cmd;
	struct common_req_info *req_info;
	const struct get_request_ops *ops;
	struct sk_buff *rskb;
	void *reply_payload;
	int reply_len;
	int ret;

	ops = get_requests[cmd];
	if (WARN_ONCE(!ops, "cmd %u has no get_request_ops\n", cmd))
		return -EOPNOTSUPP;
	req_info = ethnl_alloc_get_data(ops);
	if (!req_info)
		return -ENOMEM;
	ret = ops->parse_request(req_info, skb, info, info->nlhdr);
	if (!ops->allow_nodev_do && !req_info->dev) {
		GENL_SET_ERR_MSG(info, "device not specified in do request");
		ret = -EINVAL;
	}
	if (ret < 0)
		goto err_dev;
	ethnl_init_reply_data(req_info, ops, req_info->dev);

	rtnl_lock();
	ret = ops->prepare_data(req_info, info);
	if (ret < 0)
		goto err_rtnl;
	reply_len = ops->reply_size(req_info);
	if (ret < 0)
		goto err_rtnl;
	ret = -ENOMEM;
	rskb = ethnl_reply_init(reply_len, req_info->dev, ops->reply_cmd,
				ops->dev_attrtype, info, &reply_payload);
	if (!rskb)
		goto err_rtnl;
	ret = ops->fill_reply(rskb, req_info);
	if (ret < 0)
		goto err;
	rtnl_unlock();

	genlmsg_end(rskb, reply_payload);
	if (req_info->dev)
		dev_put(req_info->dev);
	ethnl_free_get_data(ops, req_info);
	return genlmsg_reply(rskb, info);

err:
	WARN_ONCE(ret == -EMSGSIZE,
		  "calculated message payload length (%d) not sufficient\n",
		  reply_len);
	nlmsg_free(rskb);
	ethnl_free_get_data(ops, req_info);
err_rtnl:
	rtnl_unlock();
err_dev:
	if (req_info->dev)
		dev_put(req_info->dev);
	return ret;
}

static int ethnl_get_dump_one(struct sk_buff *skb,
			      struct net_device *dev,
			      const struct get_request_ops *ops,
			      struct common_req_info *req_info)
{
	int ret;

	ethnl_init_reply_data(req_info, ops, dev);
	rtnl_lock();
	ret = ops->prepare_data(req_info, NULL);
	if (ret < 0)
		goto out;
	ret = ethnl_fill_dev(skb, dev, ops->dev_attrtype);
	if (ret < 0)
		goto out;
	ret = ops->fill_reply(skb, req_info);

out:
	rtnl_unlock();
	req_info->reply_data->dev = NULL;
	return ret;
}

/* generic ->dumpit() handler for GET requests; device iteration copied from
 * rtnl_dump_ifinfo()
 * cb->args[0]: pointer to struct get_request_ops
 * cb->args[1]: pointer to request data
 * cb->args[2]: iteration position - hashbucket
 * cb->args[3]: iteration position - ifindex
 */
int ethnl_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct net *net = sock_net(skb->sk);
	struct common_req_info *req_info;
	const struct get_request_ops *ops;
	int h, s_h, idx = 0, s_idx;
	struct hlist_head *head;
	struct net_device *dev;
	int ret = 0;
	void *ehdr;

	ops = (const struct get_request_ops *)cb->args[0];
	req_info = (struct common_req_info *)cb->args[1];
	s_h = cb->args[2];
	s_idx = cb->args[3];

	for (h = s_h; h < NETDEV_HASHENTRIES; h++, s_idx = 0) {
		idx = 0;
		head = &net->dev_index_head[h];
		hlist_for_each_entry(dev, head, index_hlist) {
			if (idx < s_idx)
				goto cont;
			ehdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid,
					   cb->nlh->nlmsg_seq,
					   &ethtool_genl_family, 0,
					   ops->reply_cmd);
			ret = ethnl_get_dump_one(skb, dev, ops, req_info);
			if (ret < 0) {
				genlmsg_cancel(skb, ehdr);
				if (ret == -EOPNOTSUPP)
					goto cont;
				if (likely(skb->len))
					goto out;
				goto out_err;
			}
			genlmsg_end(skb, ehdr);
cont:
			idx++;
		}
	}
out:
	ret = skb->len;
out_err:
	cb->args[2] = h;
	cb->args[3] = idx;
	cb->seq = net->dev_base_seq;
	nl_dump_check_consistent(cb, nlmsg_hdr(skb));

	return ret;
}

/* generic ->start() handler for GET requests */
static int ethnl_get_start(struct netlink_callback *cb)
{
	struct common_req_info *req_info;
	const struct get_request_ops *ops;
	struct genlmsghdr *ghdr;
	int ret;

	ghdr = nlmsg_data(cb->nlh);
	ops = get_requests[ghdr->cmd];
	if (WARN_ONCE(!ops, "cmd %u has no get_request_ops\n", ghdr->cmd))
		return -EOPNOTSUPP;
	req_info = ethnl_alloc_get_data(ops);
	if (!req_info)
		return -ENOMEM;

	ret = ops->parse_request(req_info, cb->skb, NULL, cb->nlh);
	if (req_info->dev) {
		/* We ignore device specification in dump requests but as the
		 * same parser as for non-dump (doit) requests is used, it
		 * would take reference to the device if it finds one
		 */
		dev_put(req_info->dev);
		req_info->dev = NULL;
	}
	if (ret < 0)
		return ret;

	cb->args[0] = (long)ops;
	cb->args[1] = (long)req_info;
	cb->args[2] = 0;
	cb->args[3] = 0;

	return 0;
}

/* generic ->done() handler for GET requests */
static int ethnl_get_done(struct netlink_callback *cb)
{
	ethnl_free_get_data((const struct get_request_ops *)cb->args[0],
			    (struct common_req_info *)cb->args[1]);

	return 0;
}

/* generic notification handler */
static void ethnl_std_notify(struct net_device *dev,
			     struct netlink_ext_ack *extack, unsigned int cmd,
			     u32 req_mask, const void *data)
{
	struct common_req_info *req_info;
	const struct get_request_ops *ops;
	struct sk_buff *skb;
	void *reply_payload;
	int reply_len;
	int ret;

	ops = get_requests[cmd - 1];
	if (WARN_ONCE(!ops, "cmd %u has no get_request_ops\n", cmd - 1))
		return;
	/* when ethnl_std_notify() is used as notify handler, command id of
	 * corresponding GET request must be one less than cmd argument passed
	 * to ethnl_std_notify()
	 */
	if (WARN_ONCE(ops->reply_cmd != cmd,
		      "reply_cmd for %u is %u, expected %u\n", cmd - 1,
		      ops->reply_cmd, cmd))
		return;

	req_info = ethnl_alloc_get_data(ops);
	if (!req_info)
		return;
	req_info->dev = dev;
	req_info->req_mask = req_mask;
	req_info->compact = true;

	ethnl_init_reply_data(req_info, ops, dev);
	ret = ops->prepare_data(req_info, NULL);
	if (ret < 0)
		goto err_data;
	reply_len = ops->reply_size(req_info);
	if (reply_len < 0)
		goto err_data;
	skb = genlmsg_new(reply_len, GFP_KERNEL);
	if (!skb)
		goto err_data;
	reply_payload = ethnl_bcastmsg_put(skb, ops->reply_cmd);
	if (!reply_payload)
		goto err_skb;

	ret = ethnl_fill_dev(skb, dev, ops->dev_attrtype);
	if (ret < 0)
		goto err_skb;
	ret = ops->fill_reply(skb, req_info);
	if (ret < 0)
		goto err_skb;
	ethnl_free_get_data(ops, req_info);
	genlmsg_end(skb, reply_payload);

	ethnl_multicast(skb, dev);
	return;

err_skb:
	nlmsg_free(skb);
err_data:
	ethnl_free_get_data(ops, req_info);
}

/* notifications */

typedef void (*ethnl_notify_handler_t)(struct net_device *dev,
				       struct netlink_ext_ack *extack,
				       unsigned int cmd, u32 req_mask,
				       const void *data);

static const ethnl_notify_handler_t ethnl_notify_handlers[] = {
};

void ethtool_notify(struct net_device *dev, struct netlink_ext_ack *extack,
		    unsigned int cmd, u32 req_mask, const void *data)
{
	if (unlikely(!ethnl_ok))
		return;
	ASSERT_RTNL();

	if (likely(cmd < ARRAY_SIZE(ethnl_notify_handlers) &&
		   ethnl_notify_handlers[cmd]))
		ethnl_notify_handlers[cmd](dev, extack, cmd, req_mask, data);
	else
		WARN_ONCE(1, "notification %u not implemented (dev=%s, req_mask=0x%x)\n",
			  cmd, netdev_name(dev), req_mask);
}
EXPORT_SYMBOL(ethtool_notify);

/* size of NEWDEV/DELDEV notification */
static inline unsigned int dev_notify_size(void)
{
	return nla_total_size(dev_ident_size());
}

static void ethnl_notify_devlist(struct netdev_notifier_info *info,
				 u16 ev_type, u16 dev_attr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(info);
	struct sk_buff *skb;
	struct nlattr *nest;
	void *ehdr;
	int ret;

	skb = genlmsg_new(dev_notify_size(), GFP_KERNEL);
	if (!skb)
		return;
	ehdr = ethnl_bcastmsg_put(skb, ETHNL_CMD_EVENT);
	if (!ehdr)
		goto out_skb;
	nest = nla_nest_start(skb, ev_type);
	if (!nest)
		goto out_skb;
	ret = ethnl_fill_dev(skb, dev, dev_attr);
	if (ret < 0)
		goto out_skb;
	nla_nest_end(skb, nest);
	genlmsg_end(skb, ehdr);

	ethnl_multicast(skb, dev);
	return;
out_skb:
	nlmsg_free(skb);
}

static int ethnl_netdev_event(struct notifier_block *this, unsigned long event,
			      void *ptr)
{
	switch (event) {
	case NETDEV_REGISTER:
		ethnl_notify_devlist(ptr, ETHTOOL_A_EVENT_NEWDEV,
				     ETHTOOL_A_NEWDEV_DEV);
		break;
	case NETDEV_UNREGISTER:
		ethnl_notify_devlist(ptr, ETHTOOL_A_EVENT_DELDEV,
				     ETHTOOL_A_DELDEV_DEV);
		break;
	case NETDEV_CHANGENAME:
		ethnl_notify_devlist(ptr, ETHTOOL_A_EVENT_RENAMEDEV,
				     ETHTOOL_A_RENAMEDEV_DEV);
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block ethnl_netdev_notifier = {
	.notifier_call = ethnl_netdev_event,
};

/* genetlink setup */

static const struct genl_ops ethtool_genl_ops[] = {
	{
		.cmd	= ETHNL_CMD_GET_STRSET,
		.doit	= ethnl_get_doit,
		.start	= ethnl_get_start,
		.dumpit	= ethnl_get_dumpit,
		.done	= ethnl_get_done,
	},
};

static const struct genl_multicast_group ethtool_nl_mcgrps[] = {
	[ETHNL_MCGRP_MONITOR] = { .name = ETHTOOL_MCGRP_MONITOR_NAME },
};

static struct genl_family ethtool_genl_family = {
	.name		= ETHTOOL_GENL_NAME,
	.version	= ETHTOOL_GENL_VERSION,
	.netnsok	= true,
	.parallel_ops	= true,
	.ops		= ethtool_genl_ops,
	.n_ops		= ARRAY_SIZE(ethtool_genl_ops),
	.mcgrps		= ethtool_nl_mcgrps,
	.n_mcgrps	= ARRAY_SIZE(ethtool_nl_mcgrps),
};

/* module setup */

static int __init ethnl_init(void)
{
	int ret;

	ret = genl_register_family(&ethtool_genl_family);
	if (WARN(ret < 0, "ethtool: genetlink family registration failed"))
		return ret;
	ethnl_ok = true;

	ret = register_netdevice_notifier(&ethnl_netdev_notifier);
	WARN(ret < 0, "ethtool: net device notifier registration failed");
	return ret;
}

subsys_initcall(ethnl_init);
