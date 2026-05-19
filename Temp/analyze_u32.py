"""
分析 LabVIEW IMAQ ColorImageToArray U32 原始数据
尝试不同的 stride 和字节序组合，找出正确的解释方式
"""
import cv2
import numpy as np
import os

TEMP_DIR = r"D:\Code\OpenCV\qr_decoder_project\Temp"
RAW_BIN = os.path.join(TEMP_DIR, "debug_raw.bin")
INFO_TXT = os.path.join(TEMP_DIR, "debug_info.txt")
OUTPUT_DIR = os.path.join(TEMP_DIR, "analysis")

os.makedirs(OUTPUT_DIR, exist_ok=True)

# 读取 info
if os.path.exists(INFO_TXT):
    with open(INFO_TXT, 'r') as f:
        print("=== debug_info.txt ===")
        print(f.read())

# 读取原始二进制
raw_data = np.fromfile(RAW_BIN, dtype=np.uint8)
print(f"\n原始数据大小: {len(raw_data)} 字节")
print(f"U32 像素数: {len(raw_data) // 4}")

# 读取info获取宽高
width, height = 720, 1280  # 默认值
if os.path.exists(INFO_TXT):
    with open(INFO_TXT, 'r') as f:
        first_line = f.readline().strip()
        for part in first_line.split():
            if part.startswith('width='):
                width = int(part.split('=')[1])
            elif part.startswith('height='):
                height = int(part.split('=')[1])

print(f"图像尺寸: {width}x{height}")
expected_bytes = width * height * 4
print(f"期望数据大小: {expected_bytes} 字节")
print(f"实际数据大小: {len(raw_data)} 字节 (仅保存了前10000像素)")

# 如果数据不够完整分析，只分析前几行
available_rows = min(len(raw_data) // (width * 4), height)
print(f"可用于完整行分析: {available_rows} 行")

# 尝试所有可能的字节序组合
# LabVIEW IMAQ U32 可能的格式:
# 1. 0x00RRGGBB -> 内存 [BB, GG, RR, 00] (小端) = BGRA
# 2. 0x00BBGGRR -> 内存 [RR, GG, BB, 00] (小端) = RGBA
# 3. 0xRRGGBB00 -> 内存 [00, BB, GG, RR] (小端) = ARGB swap
# 4. 0xBBGGRR00 -> 内存 [00, RR, GG, BB] (小端) = ABGR swap

interpretations = {
    "BGRA": [0, 1, 2, 3],   # byte[0]=B, [1]=G, [2]=R, [3]=A
    "RGBA": [2, 1, 0, 3],   # byte[0]实际是B? 不, byte[2]=R, [1]=G, [0]=B
    "ARGB_le": [3, 2, 1, 0], # 小端 0xAARRGGBB -> [BB,GG,RR,AA]
    "ABGR_le": [1, 2, 3, 0], #
}

# 更直接的方式: 尝试把4字节解释为不同通道顺序
# U32 小端: 假设值为 0x00RRGGBB, 内存中是 [BB, GG, RR, 00]
channel_orders = [
    ("B_G_R_A",  lambda b: (b[:,:,0], b[:,:,1], b[:,:,2])),  # 直接按内存顺序 B,G,R,A
    ("R_G_B_A",  lambda b: (b[:,:,2], b[:,:,1], b[:,:,0])),  # 交换R和B
    ("A_R_G_B",  lambda b: (b[:,:,3], b[:,:,2], b[:,:,1])),  # 跳过第0字节
    ("A_B_G_R",  lambda b: (b[:,:,3], b[:,:,0], b[:,:,1])),  #
]

# 只用前 available_rows 行
use_rows = min(available_rows, 200)  # 最多200行就够分析了
use_bytes = use_rows * width * 4

if len(raw_data) < use_bytes:
    print(f"\n警告: 数据不足 {use_bytes} 字节，用全部 {len(raw_data)} 字节")
    use_bytes = len(raw_data)
    use_rows = use_bytes // (width * 4)

raw_2d = raw_data[:use_bytes].reshape(use_rows, width, 4)

for name, get_bgr in channel_orders:
    b_ch, g_ch, r_ch = get_bgr(raw_2d)
    bgr_img = np.stack([b_ch, g_ch, r_ch], axis=2)

    out_path = os.path.join(OUTPUT_DIR, f"try_{name}.png")
    cv2.imwrite(out_path, bgr_img)
    mean_val = bgr_img.mean()
    nonzero = (bgr_img.max(axis=2) > 10).sum() / (use_rows * width) * 100
    print(f"  {name}: 均值={mean_val:.1f}, 非零比例={nonzero:.1f}% -> {out_path}")

# 尝试不同的 stride (行步长)
print("\n=== Stride 分析 ===")
# 当前假设 stride = width * 4 = 2880
# 尝试 stride + 4, +8, +16, +32, +64, +128 等
base_stride = width * 4
for extra in [0, 4, 8, 16, 32, 64, 128, 256, 512]:
    stride = base_stride + extra
    rows_possible = len(raw_data) // stride
    if rows_possible < 5:
        continue
    rows = min(rows_possible, use_rows)
    img = np.zeros((rows, width, 3), dtype=np.uint8)
    for r in range(rows):
        row_start = r * stride
        row_end = row_start + width * 4
        if row_end > len(raw_data):
            break
        row_data = raw_data[row_start:row_end].reshape(width, 4)
        # 用 BGRA 解释
        img[r, :, 0] = row_data[:, 0]  # B
        img[r, :, 1] = row_data[:, 1]  # G
        img[r, :, 2] = row_data[:, 2]  # R

    out_path = os.path.join(OUTPUT_DIR, f"stride_{stride}.png")
    cv2.imwrite(out_path, img)
    mean_val = img.mean()
    nonzero = (img.max(axis=2) > 10).sum() / (rows * width) * 100
    print(f"  stride={stride} (+{extra}): 均值={mean_val:.1f}, 非零比例={nonzero:.1f}% -> {out_path}")

# 也尝试 width 和 height 互换的情况
print("\n=== 宽高互换分析 ===")
swap_w, swap_h = height, width  # 1280, 720
swap_bytes = min(200, swap_h) * swap_w * 4
if len(raw_data) >= swap_bytes:
    swap_rows = min(200, swap_h)
    raw_swap = raw_data[:swap_rows * swap_w * 4].reshape(swap_rows, swap_w, 4)
    bgr_swap = raw_swap[:, :, :3]  # BGRA -> BGR
    out_path = os.path.join(OUTPUT_DIR, "swapped_wh.png")
    cv2.imwrite(out_path, bgr_swap)
    print(f"  宽高互换 (w={swap_w},h={swap_h}): 均值={bgr_swap.mean():.1f} -> {out_path}")

print(f"\n所有分析图保存到: {OUTPUT_DIR}")
print("请打开该目录查看各图片，找到颜色正确的那个！")
