package com.example.rk3288_opencv;

/**
 * 跨 UID 拍照 URI 临时授权契约（Q-CQ-B07-009）。
 *
 * 背景：拍照输出 URI 由本进程（UID A）经 FileProvider 创建，交给系统/第三方相机
 * （UID B，不同进程）。默认 B 无权限读取/写入该 URI，可能抛 SecurityException、
 * 返回空文件或拍照失败。必须向 ACTION_IMAGE_CAPTURE 的 Intent 追加
 * FLAG_GRANT_READ_URI_PERMISSION | FLAG_GRANT_WRITE_URI_PERMISSION，并附带 ClipData
 * （API 16+ 的授权传递载体），把临时读写权限授予接收方；拍照消费完成后必须调用
 * revokeUriPermission 及时撤销，避免授权残留到进程结束。
 *
 * 授权时序：
 *   1) dispatchTakePictureIntent：buildTakePictureGrant(uri) → flags + ClipData URI，
 *      授予跨 UID 相机临时读写权限；
 *   2) handleSystemCameraResult / onActivityResult：消费完成，revokeUriPermission 撤销，
 *      授予与撤销必须使用同一掩码 GRANT_MASK 才能精确回收。
 *
 * 本类保持纯 Java（不依赖 android.* 框架类），使 JVM 单测可在无 Robolectric 环境下
 * 验证授权/撤销的位掩码语义；生产代码与单测共用同一常量与判定函数，保证契约单一来源。
 */
public final class PhotoUriGrant {

    /** 与 Intent.FLAG_GRANT_READ_URI_PERMISSION 同值（Android ABI 稳定常量 = 1） */
    public static final int FLAG_GRANT_READ = 0x1;
    /** 与 Intent.FLAG_GRANT_WRITE_URI_PERMISSION 同值（Android ABI 稳定常量 = 2） */
    public static final int FLAG_GRANT_WRITE = 0x2;
    /** 读写组合掩码：grantUriPermission 与 revokeUriPermission 必须使用同一掩码 */
    public static final int GRANT_MASK = FLAG_GRANT_READ | FLAG_GRANT_WRITE;
    /** ClipData 标签：仅作为授权 URI 的冗余载体（API 16+ 依赖 data/clipData 传递授权） */
    public static final String CLIP_DATA_LABEL = "photo-output";

    private PhotoUriGrant() {
    }

    /**
     * 判定 intent flags 是否已携带跨 UID 读写授权。
     * 修复前 dispatchTakePictureIntent 未设置任何 FLAG_GRANT_*（flags=0），
     * 跨 UID 相机读写必然失败——这是本缺陷的 RED 判别基准。
     */
    public static boolean hasReadWriteGrant(int intentFlags) {
        return (intentFlags & GRANT_MASK) == GRANT_MASK;
    }

    /**
     * 判定 ClipData 载荷是否携带拍照输出 URI。
     * 仅 EXTRA_OUTPUT 不被所有相机实现认可，必须冗余放入 ClipData 才能跨 Intent 传递授权。
     */
    public static boolean clipDataCarriesUri(String clipDataUri, String photoUri) {
        return photoUri != null && photoUri.equals(clipDataUri);
    }

    /**
     * 授权生命周期不变式：已授予且未撤销即为残留。
     * 修复前消费路径缺 revoke 步骤 → granted=true, revoked=false → 残留（RED）；
     * 修复后消费完成路径必然撤销 → 无残留。
     */
    public static boolean hasResidualAfter(boolean granted, boolean revoked) {
        return granted && !revoked;
    }

    /** 授权载荷：供生产代码组装拍照 Intent 的 flags 与 ClipData URI（纯数据，便于单测） */
    public static final class GrantData {
        public final int flags;
        public final String uri;

        GrantData(int flags, String uri) {
            this.flags = flags;
            this.uri = uri;
        }
    }

    /** 依据拍照输出 URI 生成跨 UID 读写授权载荷（单一来源，生产与单测共用） */
    public static GrantData buildTakePictureGrant(String photoUri) {
        return new GrantData(GRANT_MASK, photoUri);
    }
}
