#ifndef VCE_LIVERPOOL_SPS_H
#define VCE_LIVERPOOL_SPS_H

struct vce_liverpool_bits {
	u8 data[512];
	u32 count;
	u32 position;
	bool error;
};

static u32 vce_liverpool_get_bits(struct vce_liverpool_bits *bits, u32 count)
{
	u32 result = 0, i;

	if (count > 32 || bits->position + count > bits->count) {
		bits->error = true;
		return 0;
	}
	for (i = 0; i < count; i++) {
		result = (result << 1) |
			((bits->data[bits->position / 8] >> (7 - bits->position % 8)) & 1);
		bits->position++;
	}
	return result;
}

static void vce_liverpool_put_bits(struct vce_liverpool_bits *bits, u32 value, u32 count)
{
	u32 i;

	if (count > 32 || bits->count + count > sizeof(bits->data) * 8) {
		bits->error = true;
		return;
	}
	for (i = 0; i < count; i++) {
		u32 bit = (value >> (count - i - 1)) & 1;

		bits->data[bits->count / 8] |= bit << (7 - bits->count % 8);
		bits->count++;
	}
}

static u32 vce_liverpool_get_ue(struct vce_liverpool_bits *bits)
{
	u32 zeroes = 0;

	while (!bits->error && !vce_liverpool_get_bits(bits, 1)) {
		if (++zeroes > 30) {
			bits->error = true;
			return 0;
		}
	}
	return ((1u << zeroes) - 1) + vce_liverpool_get_bits(bits, zeroes);
}

static void vce_liverpool_put_ue(struct vce_liverpool_bits *bits, u32 value)
{
	u32 count = 0, code;

	if (value >= 0x7fffffff) {
		bits->error = true;
		return;
	}
	code = ++value;
	while (code >>= 1)
		count++;
	vce_liverpool_put_bits(bits, 0, count);
	vce_liverpool_put_bits(bits, value, count + 1);
}

static int vce_liverpool_fix_sps(const u8 *source, u32 size, u8 *output,
		u32 capacity, u32 width, u32 height, u32 fps_num, u32 fps_den,
		u32 requested_profile)
{
	struct vce_liverpool_bits input = {}, result = {};
	u32 i, offset = 0, zeros = 0, prefix_bits, cropped_height, cropped_width;
	u32 profile, poc_type, frame_only, native_width, native_height;
	u32 sps_id_end;

	if (size < 5 || size > sizeof(input.data) || !fps_num || !fps_den ||
	    fps_num > 1000000 || fps_den > 1000000)
		return -EINVAL;
	for (i = 1; i < size; i++) {
		if (zeros == 2 && source[i] == 3) {
			zeros = 0;
			continue;
		}
		input.data[offset++] = source[i];
		zeros = source[i] ? 0 : zeros + 1;
	}
	input.count = offset * 8;
	profile = vce_liverpool_get_bits(&input, 8);
	if (profile != 66 && profile != 77)
		return -EINVAL;
	if (requested_profile != profile && !(requested_profile == 100 && profile == 77))
		return -EINVAL;
	vce_liverpool_get_bits(&input, 16);
	vce_liverpool_get_ue(&input);
	sps_id_end = input.position;
	vce_liverpool_get_ue(&input);
	poc_type = vce_liverpool_get_ue(&input);
	if (!poc_type)
		vce_liverpool_get_ue(&input);
	else if (poc_type != 2)
		return -EINVAL;
	vce_liverpool_get_ue(&input);
	vce_liverpool_get_bits(&input, 1);
	native_width = (vce_liverpool_get_ue(&input) + 1) * 16;
	native_height = (vce_liverpool_get_ue(&input) + 1) * 16;
	frame_only = vce_liverpool_get_bits(&input, 1);
	if (!frame_only)
		return -EINVAL;
	vce_liverpool_get_bits(&input, 1);
	if (input.error || native_width != width || native_height < height ||
	    native_height - height > 14 || (height & 1))
		return -EINVAL;
	prefix_bits = input.position;
	for (i = 0; i < prefix_bits; i++) {
		if (i == sps_id_end && requested_profile == 100) {
			vce_liverpool_put_ue(&result, 1);
			vce_liverpool_put_ue(&result, 0);
			vce_liverpool_put_ue(&result, 0);
			vce_liverpool_put_bits(&result, 0, 1);
			vce_liverpool_put_bits(&result, 0, 1);
		}
		vce_liverpool_put_bits(&result,
			(input.data[i / 8] >> (7 - i % 8)) & 1, 1);
	}
	result.data[0] = requested_profile;
	if (requested_profile == 100)
		result.data[1] = 0;
	if (profile == 66)
		result.data[1] |= 0x40;
	cropped_height = (native_height - height) / 2;
	cropped_width = (native_width - width) / 2;
	vce_liverpool_put_bits(&result, !!(cropped_width || cropped_height), 1);
	if (cropped_width || cropped_height) {
		vce_liverpool_put_ue(&result, 0);
		vce_liverpool_put_ue(&result, cropped_width);
		vce_liverpool_put_ue(&result, 0);
		vce_liverpool_put_ue(&result, cropped_height);
	}
	vce_liverpool_put_bits(&result, 1, 1);
	vce_liverpool_put_bits(&result, 1, 1);
	vce_liverpool_put_bits(&result, 1, 8);
	vce_liverpool_put_bits(&result, 0, 1);
	vce_liverpool_put_bits(&result, 1, 1);
	vce_liverpool_put_bits(&result, 5, 3);
	vce_liverpool_put_bits(&result, 0, 1);
	vce_liverpool_put_bits(&result, 1, 1);
	vce_liverpool_put_bits(&result, 2, 8);
	vce_liverpool_put_bits(&result, 2, 8);
	vce_liverpool_put_bits(&result, 2, 8);
	vce_liverpool_put_bits(&result, 0, 1);
	vce_liverpool_put_bits(&result, 1, 1);
	vce_liverpool_put_bits(&result, fps_den, 32);
	vce_liverpool_put_bits(&result, fps_num * 2, 32);
	vce_liverpool_put_bits(&result, 1, 1);
	vce_liverpool_put_bits(&result, 0, 1);
	vce_liverpool_put_bits(&result, 0, 1);
	vce_liverpool_put_bits(&result, 0, 1);
	vce_liverpool_put_bits(&result, 1, 1);
	vce_liverpool_put_bits(&result, 1, 1);
	vce_liverpool_put_ue(&result, 2);
	vce_liverpool_put_ue(&result, 1);
	vce_liverpool_put_ue(&result, 14);
	vce_liverpool_put_ue(&result, 14);
	vce_liverpool_put_ue(&result, 0);
	vce_liverpool_put_ue(&result, 1);
	vce_liverpool_put_bits(&result, 1, 1);
	while (result.count % 8)
		vce_liverpool_put_bits(&result, 0, 1);
	if (result.error || capacity < 2)
		return -EINVAL;
	output[0] = source[0];
	offset = 1;
	zeros = 0;
	for (i = 0; i < result.count / 8; i++) {
		u8 value = result.data[i];

		if (offset + 2 > capacity)
			return -ENOSPC;
		if (zeros == 2 && value <= 3) {
			output[offset++] = 3;
			zeros = 0;
		}
		output[offset++] = value;
		zeros = value ? 0 : zeros + 1;
	}
	return offset;
}

#endif
