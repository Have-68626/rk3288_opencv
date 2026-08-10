package com.example.rk3288_opencv;

import org.junit.Test;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

/**
 * 跨 UID 拍照 URI 临时授权契约测试（Q-CQ-B07-009）。
 *
 * RED 判别逻辑：
 * 1) 授予（跨 UID 相机可读写）：修复前 dispatchTakePictureIntent
 *    （CameraFragment.java:398-402 / MainActivity.java:1453-1457）仅把 FileProvider URI
 *    放入 EXTRA_OUTPUT，未设置 FLAG_GRANT_READ/WRITE 且无 ClipData → 接收方 flags=0，
 *    不同 UID 的相机进程读取/写入该 URI 必然失败。本测试断言生产路径所用的授权载荷
 *    （buildTakePictureGrant 单一来源）必须携带读写掩码与拍照 URI。
 * 2) 撤销（使用后不残留）：修复前 handleSystemCameraResult 从不调用 revokeUriPermission，
 *    临时授权残留到进程结束。本测试断言撤销掩码与授予掩码一致，且“已授予但未撤销即残留”
 *    不变式成立——修复须保证消费完成路径必然走到撤销。
 */
public class PhotoUriGrantTest {

    // ── RED 判别 1：授予跨 UID 读写权限 ──

    @Test
    public void testTakePictureGrantCarriesReadWrite() {
        String photoUri = "content://com.example.rk3288_opencv.fileprovider/cache-mock/mock_capture_20260808_a.jpg";
        PhotoUriGrant.GrantData grant = PhotoUriGrant.buildTakePictureGrant(photoUri);
        assertTrue("授权载荷必须携带跨 UID 读写标志（修复前 flags=0，跨 UID 相机不可写）",
                PhotoUriGrant.hasReadWriteGrant(grant.flags));
        assertTrue("授权掩码必须同时包含读位与写位",
                PhotoUriGrant.hasReadWriteGrant(PhotoUriGrant.GRANT_MASK));
    }

    @Test
    public void testNoGrantIsDenied_redBaseline() {
        // 修复前：intent 未携带任何 FLAG_GRANT_* → flags=0 → 跨 UID 相机读写被拒
        assertFalse("无授权标志时不得判定为可读写（修复前语义）",
                PhotoUriGrant.hasReadWriteGrant(0));
        // 仅读或仅写都不够：相机必须向 EXTRA_OUTPUT 写入文件并回读结果
        assertFalse("仅读位不足以写入",
                PhotoUriGrant.hasReadWriteGrant(PhotoUriGrant.FLAG_GRANT_READ));
        assertFalse("仅写位不足以读取",
                PhotoUriGrant.hasReadWriteGrant(PhotoUriGrant.FLAG_GRANT_WRITE));
    }

    @Test
    public void testClipDataMustCarryPhotoUri() {
        String photoUri = "content://com.example.rk3288_opencv.fileprovider/cache-mock/mock_capture_20260808_b.jpg";
        assertTrue("ClipData 必须携带拍照输出 URI（API 16+ 授权传递载体）",
                PhotoUriGrant.clipDataCarriesUri(photoUri, photoUri));
        assertFalse("ClipData 载荷与拍照 URI 不一致时必须拒绝（防止错授其他 URI）",
                PhotoUriGrant.clipDataCarriesUri("content://other.authority/img.jpg", photoUri));
        assertFalse("空 ClipData 应视为未授权",
                PhotoUriGrant.clipDataCarriesUri(null, photoUri));
    }

    // ── RED 判别 2：使用后及时撤销，不残留 ──

    @Test
    public void testRevokeMaskMustMatchGrantMask() {
        // 撤销必须使用与授予一致的掩码，否则无法精确回收已授的读写权限
        assertTrue("撤销掩码必须覆盖读位",
                (PhotoUriGrant.GRANT_MASK & PhotoUriGrant.FLAG_GRANT_READ) != 0);
        assertTrue("撤销掩码必须覆盖写位",
                (PhotoUriGrant.GRANT_MASK & PhotoUriGrant.FLAG_GRANT_WRITE) != 0);
    }

    @Test
    public void testGrantedWithoutRevokeLeavesResidual_redBaseline() {
        // 修复前：消费路径从不 revokeUriPermission → 已授予且未撤销 → 授权残留（RED 基线）。
        // 修复后：handleSystemCameraResult/onActivityResult 消费完成必撤销 → 无残留。
        assertTrue("未撤销即残留（修复前缺陷语义）",
                PhotoUriGrant.hasResidualAfter(true, false));
        assertFalse("撤销后不得残留",
                PhotoUriGrant.hasResidualAfter(true, true));
        assertFalse("从未授予则不存在残留",
                PhotoUriGrant.hasResidualAfter(false, false));
    }
}
