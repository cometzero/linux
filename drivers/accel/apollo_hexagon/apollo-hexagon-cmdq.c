// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apollo Hexagon command queue helpers.
 */

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include <drm/drm_gem.h>

#include "apollo-hexagon.h"

struct apollo_hexagon_loaded_exec {
	bool valid;
	bool payload_valid;
	bool code_valid;
	u32 entry_kind;
	u32 input_bytes;
	u32 output_bytes;
	u32 payload_opcode;
	u32 payload_code_words;
	u32 payload_entry_word;
	u32 payload_end_word;
};

const char *apollo_hexagon_exec_kind_name(u32 entry_kind)
{
	switch (entry_kind) {
	case APOLLO_HEXAGON_EXEC_KIND_CNN:
		return "CNN";
	case APOLLO_HEXAGON_EXEC_KIND_VADD:
		return "VADD";
	case APOLLO_HEXAGON_EXEC_KIND_MNIST:
		return "MNIST";
	default:
		return "unknown";
	}
}

static void apollo_hexagon_cmdq_write_packet(struct apollo_hexagon *test,
					     const u32 *packet)
{
	u32 i;

	for (i = 0; i < APOLLO_HEXAGON_CMDQ_PACKET_WORDS; i++)
		writel(packet[i],
		       test->shared_base + APOLLO_HEXAGON_CMDQ_OFFSET +
		       i * sizeof(u32));
}

void apollo_hexagon_cmdq_write_buffer(struct apollo_hexagon *test,
				      const u32 *command, u32 bytes)
{
	u32 words = bytes / sizeof(u32);
	u32 i;

	for (i = 0; i < words; i++)
		writel(command[i],
		       test->shared_base + APOLLO_HEXAGON_CMDQ_OFFSET +
		       i * sizeof(u32));
}

void apollo_hexagon_cmdq_write_dispatch(struct apollo_hexagon *test,
					u32 entry_kind, u64 input_iova,
					u64 output_iova, u32 input_bytes,
					u32 output_bytes)
{
	u32 packet[APOLLO_HEXAGON_CMDQ_PACKET_WORDS] = {
		APOLLO_HEXAGON_CMDQ_OPCODE_DISPATCH,
		entry_kind,
		lower_32_bits(input_iova),
		upper_32_bits(input_iova),
		lower_32_bits(output_iova),
		upper_32_bits(output_iova),
		input_bytes,
		output_bytes,
	};

	apollo_hexagon_cmdq_write_packet(test, packet);
}

static bool apollo_hexagon_cmdq_kind_sizes_valid(u32 entry_kind,
						 u32 input_bytes,
						 u32 output_bytes)
{
	switch (entry_kind) {
	case APOLLO_HEXAGON_EXEC_KIND_CNN:
		return input_bytes ==
			       APOLLO_HEXAGON_CNN_INPUT_WORDS * sizeof(u32) &&
		       output_bytes ==
			       APOLLO_HEXAGON_CNN_OUTPUT_WORDS * sizeof(u32);
	case APOLLO_HEXAGON_EXEC_KIND_VADD:
		return input_bytes ==
			       APOLLO_HEXAGON_VADD_INPUT_WORDS * sizeof(u32) &&
		       output_bytes ==
			       APOLLO_HEXAGON_VADD_OUTPUT_WORDS * sizeof(u32);
	case APOLLO_HEXAGON_EXEC_KIND_MNIST:
		return input_bytes ==
			       APOLLO_HEXAGON_MNIST_INPUT_WORDS * sizeof(u32) &&
		       output_bytes ==
			       APOLLO_HEXAGON_MNIST_OUTPUT_WORDS * sizeof(u32);
	default:
		return false;
	}
}

static bool apollo_hexagon_cmdq_load_exec_is_valid(const u32 *packet)
{
	u32 slot = packet[1];
	u32 entry_kind = packet[5];
	u32 input_bytes = packet[6];
	u32 output_bytes = packet[7];

	if (slot == 0 || slot > APOLLO_HEXAGON_CMDQ_EXEC_SLOT_MAX)
		return false;
	if (packet[2] != APOLLO_HEXAGON_APKO_MAGIC ||
	    packet[3] != APOLLO_HEXAGON_APKO_ABI_VERSION ||
	    packet[4] != APOLLO_HEXAGON_EXEC_FORMAT_APKO_V0)
		return false;
	return apollo_hexagon_cmdq_kind_sizes_valid(entry_kind, input_bytes,
						    output_bytes);
}

static bool apollo_hexagon_cmdq_payload_opcode_valid(u32 payload_opcode)
{
	return payload_opcode == APOLLO_HEXAGON_EXEC_KIND_CNN ||
	       payload_opcode == APOLLO_HEXAGON_EXEC_KIND_VADD ||
	       payload_opcode == APOLLO_HEXAGON_EXEC_KIND_MNIST;
}

static bool apollo_hexagon_apko_code_entry_kind(u32 code_entry, u32 *entry_kind)
{
	u32 kind;

	if ((code_entry & APOLLO_HEXAGON_APKO_CODE_OP_MASK) !=
	    APOLLO_HEXAGON_APKO_CODE_OP_MODEL_DISPATCH)
		return false;

	kind = code_entry & APOLLO_HEXAGON_APKO_CODE_MODEL_MASK;
	if (!apollo_hexagon_cmdq_payload_opcode_valid(kind))
		return false;

	*entry_kind = kind;
	return true;
}

static bool apollo_hexagon_apko_code_program_kind(const u32 *code_words,
						  u32 word_count,
						  u32 *entry_kind)
{
	if (word_count != APOLLO_HEXAGON_APKO_CODE_PROGRAM_WORDS)
		return false;
	if (code_words[1] != APOLLO_HEXAGON_APKO_CODE_OP_END)
		return false;
	return apollo_hexagon_apko_code_entry_kind(code_words[0], entry_kind);
}

static bool apollo_hexagon_cmdq_load_payload_is_valid(
	const u32 *packet, const struct apollo_hexagon_loaded_exec *loaded)
{
	u32 slot = packet[1];
	u32 payload_opcode = packet[4];

	if (slot == 0 || slot > APOLLO_HEXAGON_CMDQ_EXEC_SLOT_MAX)
		return false;
	if (!loaded[slot].valid)
		return false;
	if (packet[2] != APOLLO_HEXAGON_APKO_PAYLOAD_MAGIC ||
	    packet[3] != APOLLO_HEXAGON_APKO_PAYLOAD_VERSION ||
	    packet[5] != APOLLO_HEXAGON_APKO_PAYLOAD_DESCRIPTOR_WORDS)
		return false;
	if (packet[6] != APOLLO_HEXAGON_APKO_CODE_PROGRAM_WORDS ||
	    packet[7] != 0)
		return false;
	if (!apollo_hexagon_cmdq_payload_opcode_valid(payload_opcode))
		return false;
	return payload_opcode == loaded[slot].entry_kind;
}

static bool apollo_hexagon_cmdq_load_code_is_valid(
	const u32 *packet, const struct apollo_hexagon_loaded_exec *loaded)
{
	u32 slot = packet[1];
	u32 word_offset = packet[4];
	u32 word_count = packet[5];
	u32 code_words[APOLLO_HEXAGON_APKO_CODE_PROGRAM_WORDS] = {
		packet[6], packet[7]
	};
	u32 entry_kind;

	if (slot == 0 || slot > APOLLO_HEXAGON_CMDQ_EXEC_SLOT_MAX)
		return false;
	if (!loaded[slot].valid || !loaded[slot].payload_valid)
		return false;
	if (packet[2] != APOLLO_HEXAGON_APKO_CODE_MAGIC ||
	    packet[3] != APOLLO_HEXAGON_APKO_CODE_VERSION)
		return false;
	if (word_offset != 0 || word_count != loaded[slot].payload_code_words)
		return false;
	if (!apollo_hexagon_apko_code_program_kind(code_words, word_count,
						  &entry_kind))
		return false;
	return entry_kind == loaded[slot].payload_opcode;
}

static bool apollo_hexagon_cmdq_dispatch_uses_loaded_exec(
	const u32 *packet, const struct apollo_hexagon_loaded_exec *loaded)
{
	u32 slot;

	if (!(packet[1] & APOLLO_HEXAGON_CMDQ_DISPATCH_EXEC_SLOT_FLAG))
		return false;

	slot = packet[1] & ~APOLLO_HEXAGON_CMDQ_DISPATCH_EXEC_SLOT_FLAG;
	if (slot == 0 || slot > APOLLO_HEXAGON_CMDQ_EXEC_SLOT_MAX)
		return false;
	if (!loaded[slot].valid || !loaded[slot].payload_valid ||
	    !loaded[slot].code_valid)
		return false;
	return packet[6] == loaded[slot].input_bytes &&
	       packet[7] == loaded[slot].output_bytes;
}

static bool apollo_hexagon_cmdq_packet_is_bound_dispatch(
	const u32 *packet, const struct apollo_hexagon_loaded_exec *loaded,
	u32 *entry_kind)
{
	u32 code_words[APOLLO_HEXAGON_APKO_CODE_PROGRAM_WORDS];
	u32 slot;

	if (packet[0] != APOLLO_HEXAGON_CMDQ_OPCODE_DISPATCH)
		return false;
	if (packet[1] == APOLLO_HEXAGON_CMDQ_DISPATCH_KIND_CNN ||
	    packet[1] == APOLLO_HEXAGON_CMDQ_DISPATCH_KIND_VADD ||
	    packet[1] == APOLLO_HEXAGON_CMDQ_DISPATCH_KIND_MNIST) {
		*entry_kind = packet[1];
		return true;
	}
	if (!apollo_hexagon_cmdq_dispatch_uses_loaded_exec(packet, loaded))
		return false;

	slot = packet[1] & ~APOLLO_HEXAGON_CMDQ_DISPATCH_EXEC_SLOT_FLAG;
	code_words[0] = loaded[slot].payload_entry_word;
	code_words[1] = loaded[slot].payload_end_word;
	return apollo_hexagon_apko_code_program_kind(
		code_words, loaded[slot].payload_code_words, entry_kind);
}

static u64 apollo_hexagon_cmdq_packet_iova(u32 lo, u32 hi)
{
	return ((u64)hi << 32) | lo;
}

static int apollo_hexagon_cmdq_prepare_bound_dispatch_packet(
	struct device *dev, struct apollo_hexagon_context *ctx, u32 *packet,
	const struct apollo_hexagon_loaded_exec *loaded,
	struct apollo_hexagon_bound_dispatch *bound)
{
	u64 input_iova;
	u64 output_iova;
	u32 entry_kind = 0;
	u32 input_bytes;
	u32 output_bytes;
	struct drm_gem_object *input_obj;
	u64 input_offset;
	int ret;

	if (!apollo_hexagon_cmdq_packet_is_bound_dispatch(packet, loaded,
							  &entry_kind))
		return 0;
	if (bound->active)
		return -EINVAL;

	input_iova = apollo_hexagon_cmdq_packet_iova(packet[2], packet[3]);
	output_iova = apollo_hexagon_cmdq_packet_iova(packet[4], packet[5]);
	input_bytes = packet[6];
	output_bytes = packet[7];
	if (!apollo_hexagon_cmdq_kind_sizes_valid(entry_kind, input_bytes,
						  output_bytes))
		return 0;

	ret = apollo_hexagon_context_get_iova_bo(
		ctx, input_iova, input_bytes, APOLLO_HEXAGON_BO_BIND_USAGE_READ,
		&input_obj, &input_offset);
	if (ret == -ENOENT)
		return 0;
	if (ret)
		return ret;

	ret = apollo_hexagon_context_get_iova_bo(
		ctx, output_iova, output_bytes, APOLLO_HEXAGON_BO_BIND_USAGE_WRITE,
		&bound->output_obj, &bound->output_offset);
	if (ret == -ENOENT) {
		drm_gem_object_put(input_obj);
		return 0;
	}
	if (ret)
		goto out_put_input;

	bound->active = true;
	bound->packet = packet;
	bound->entry_kind = entry_kind;
	bound->input_obj = input_obj;
	bound->input_iova = input_iova;
	bound->output_iova = output_iova;
	bound->input_offset = input_offset;
	bound->input_bytes = input_bytes;
	bound->output_bytes = output_bytes;
	dev_info(dev,
		 "command BO bound %s dispatch ctx=%u input=0x%llx output=0x%llx bytes=%u/%u direct-tbu=yes\n",
		 apollo_hexagon_exec_kind_name(entry_kind), ctx->handle,
		 input_iova, output_iova, input_bytes, output_bytes);

	return 0;

out_put_input:
	drm_gem_object_put(input_obj);
	return ret;
}

int apollo_hexagon_cmdq_prepare_bound_dispatch(
	struct device *dev, struct apollo_hexagon_context *ctx, u32 *command,
	u32 command_size, struct apollo_hexagon_bound_dispatch *bound)
{
	struct apollo_hexagon_loaded_exec loaded[APOLLO_HEXAGON_CMDQ_EXEC_SLOT_MAX + 1] = { 0 };
	u32 packet_count = command_size / APOLLO_HEXAGON_CMDQ_PACKET_BYTES;
	u32 packet_index;
	int ret;

	memset(bound, 0, sizeof(*bound));
	for (packet_index = 0; packet_index < packet_count; packet_index++) {
		u32 *packet = command +
			      packet_index * APOLLO_HEXAGON_CMDQ_PACKET_WORDS;
		u32 slot;

		if (packet[0] == APOLLO_HEXAGON_CMDQ_OPCODE_LOAD_EXECUTABLE &&
		    apollo_hexagon_cmdq_load_exec_is_valid(packet)) {
			slot = packet[1];
			loaded[slot].valid = true;
			loaded[slot].payload_valid = false;
			loaded[slot].code_valid = false;
			loaded[slot].entry_kind = packet[5];
			loaded[slot].input_bytes = packet[6];
			loaded[slot].output_bytes = packet[7];
			loaded[slot].payload_opcode = 0;
			loaded[slot].payload_code_words = 0;
			loaded[slot].payload_entry_word = 0;
			loaded[slot].payload_end_word = 0;
			dev_info(dev,
				 "command BO LOAD_EXECUTABLE slot=%u kind=%u input=%u output=%u\n",
				 slot, packet[5], packet[6], packet[7]);
			continue;
		}

		if (packet[0] == APOLLO_HEXAGON_CMDQ_OPCODE_LOAD_PAYLOAD &&
		    apollo_hexagon_cmdq_load_payload_is_valid(packet, loaded)) {
			slot = packet[1];
			loaded[slot].payload_valid = true;
			loaded[slot].code_valid = false;
			loaded[slot].payload_opcode = packet[4];
			loaded[slot].payload_code_words = packet[6];
			loaded[slot].payload_entry_word = 0;
			loaded[slot].payload_end_word = 0;
			dev_info(dev,
				 "command BO LOAD_PAYLOAD slot=%u opcode=%u words=%u code_words=%u\n",
				 slot, packet[4], packet[5], packet[6]);
			continue;
		}

		if (packet[0] == APOLLO_HEXAGON_CMDQ_OPCODE_LOAD_CODE &&
		    apollo_hexagon_cmdq_load_code_is_valid(packet, loaded)) {
			slot = packet[1];
			loaded[slot].code_valid = true;
			loaded[slot].payload_entry_word = packet[6];
			loaded[slot].payload_end_word = packet[7];
			dev_info(dev,
				 "command BO LOAD_CODE slot=%u offset=%u words=%u entry_word=%u end_word=%u\n",
				 slot, packet[4], packet[5], packet[6],
				 packet[7]);
			continue;
		}

		ret = apollo_hexagon_cmdq_prepare_bound_dispatch_packet(
			dev, ctx, packet, loaded, bound);
		if (ret)
			return ret;
	}

	return 0;
}

int apollo_hexagon_cmdq_map_bound_dispatch(
	struct device *dev, struct apollo_hexagon *test,
	struct apollo_hexagon_bound_dispatch *bound)
{
	int ret;

	if (!bound->active)
		return 0;

	ret = apollo_hexagon_bo_map_tbu(
		dev, test, bound->input_obj, bound->input_offset,
		bound->input_iova, bound->input_bytes, DMA_TO_DEVICE,
		&bound->input_map);
	if (ret)
		return ret;

	ret = apollo_hexagon_bo_map_tbu(
		dev, test, bound->output_obj, bound->output_offset,
		bound->output_iova, bound->output_bytes, DMA_FROM_DEVICE,
		&bound->output_map);
	if (ret) {
		apollo_hexagon_bo_unmap_tbu(
			dev, test, &bound->input_map);
		return ret;
	}

	dev_info(dev,
		 "command BO bound %s dispatch uses BO IOVA input=0x%llx output=0x%llx\n",
		 apollo_hexagon_exec_kind_name(bound->entry_kind),
		 bound->input_iova, bound->output_iova);
	return 0;
}

void apollo_hexagon_cmdq_sync_bound_dispatch_for_cpu(
	struct device *dev, struct apollo_hexagon_bound_dispatch *bound)
{
	if (!bound->active)
		return;

	apollo_hexagon_bo_sync_tbu_for_cpu(dev, &bound->output_map);
}

void apollo_hexagon_cmdq_unmap_bound_dispatch(
	struct device *dev, struct apollo_hexagon *test,
	struct apollo_hexagon_bound_dispatch *bound)
{
	if (!bound->active)
		return;

	apollo_hexagon_bo_unmap_tbu(dev, test, &bound->output_map);
	apollo_hexagon_bo_unmap_tbu(dev, test, &bound->input_map);
}

void apollo_hexagon_cmdq_bound_dispatch_put(
	struct apollo_hexagon_bound_dispatch *bound)
{
	if (bound->input_obj)
		drm_gem_object_put(bound->input_obj);
	if (bound->output_obj)
		drm_gem_object_put(bound->output_obj);
	memset(bound, 0, sizeof(*bound));
}

int apollo_hexagon_cmdq_wait(struct device *dev, struct apollo_hexagon *test,
			     u32 queue_id, u32 expected_head, u32 *fence_seq,
			     u32 *final_status, u32 *final_fault,
			     u32 *final_head)
{
	u32 status = APOLLO_HEXAGON_CMDQ_STATUS_ERROR;
	u32 fault = APOLLO_HEXAGON_CMDQ_FAULT_NONE;
	u32 fence = 0;
	u32 head = 0;
	u32 irq;
	unsigned long timeout;
	int tries;

	status = readl(test->regs + APOLLO_HEXAGON_REG_CMDQ_STATUS);
	irq = readl(test->regs + APOLLO_HEXAGON_REG_IRQ_STATUS) |
	      atomic_read(&test->async_irq_pending);
	if (test->doorbell_irq >= 0 &&
	    status != APOLLO_HEXAGON_CMDQ_STATUS_DONE &&
	    status != APOLLO_HEXAGON_CMDQ_STATUS_ERROR &&
	    !(irq & BIT(queue_id))) {
		timeout = wait_for_completion_timeout(
			&test->async_fence,
			msecs_to_jiffies(APOLLO_HEXAGON_ACCEL_TIMEOUT_MS));
		if (timeout) {
			dev_info(dev,
				 "command BO async fence irq wait signaled queue=%u irq=%d\n",
				 queue_id, test->doorbell_irq);
		} else {
			dev_warn(dev,
				 "command BO async fence irq wait timeout queue=%u irq=%d; polling fallback\n",
				 queue_id, test->doorbell_irq);
		}
	}

	for (tries = 0;
	     tries < APOLLO_HEXAGON_ACCEL_TIMEOUT_MS /
		     APOLLO_HEXAGON_ACCEL_POLL_MS;
	     tries++) {
		status = readl(test->regs + APOLLO_HEXAGON_REG_CMDQ_STATUS);
		if (status == APOLLO_HEXAGON_CMDQ_STATUS_DONE ||
		    status == APOLLO_HEXAGON_CMDQ_STATUS_ERROR)
			break;
		msleep(APOLLO_HEXAGON_ACCEL_POLL_MS);
	}

	fault = readl(test->regs + APOLLO_HEXAGON_REG_CMDQ_FAULT_CODE);
	head = readl(test->regs + APOLLO_HEXAGON_REG_CMDQ_HEAD);
	fence = readl(test->regs + APOLLO_HEXAGON_REG_CMDQ_FENCE_VALUE);
	if (final_status)
		*final_status = status;
	if (final_fault)
		*final_fault = fault;
	if (final_head)
		*final_head = head;
	if (fence_seq)
		*fence_seq = fence;
	if (status != APOLLO_HEXAGON_CMDQ_STATUS_DONE ||
	    fault != APOLLO_HEXAGON_CMDQ_FAULT_NONE ||
	    head != expected_head) {
		dev_err(dev,
			"command BO submit failed queue=%u status=0x%x fault=0x%x head=0x%x expected=0x%x\n",
			queue_id, status, fault, head, expected_head);
		return -EIO;
	}

	irq = readl(test->regs + APOLLO_HEXAGON_REG_IRQ_STATUS) |
	      atomic_read(&test->async_irq_pending);
	if (!(irq & BIT(queue_id))) {
		dev_err(dev, "command BO async irq missing queue=%u irq=0x%x\n",
			queue_id, irq);
		return -EIO;
	}

	atomic_andnot(BIT(queue_id), &test->async_irq_pending);
	writel(BIT(queue_id), test->regs + APOLLO_HEXAGON_REG_IRQ_ACK);
	dev_info(dev,
		 "command BO fence signaled queue=%u fence=%u irq=0x%x\n",
		 queue_id, fence, irq);

	return 0;
}
