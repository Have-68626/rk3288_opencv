package com.example.rk3288_opencv;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import java.io.File;
import java.util.concurrent.ConcurrentHashMap;

/**
 * 文件存储的**共享小工具**（任务 A4c）：把此前在两个写入器中逐字重复的实现收敛到一处。
 *
 * <p>覆盖范围刻意只含**无状态、语义等价**的两项：
 * <ul>
 *   <li>{@link #renameQuietly} —— {@code FaceModelManager} 与 {@code AndroidEnrollmentRepository}
 *       各有一份逐字相同（仅注释不同）的实现；</li>
 *   <li>{@link #dirLockFor} —— 两份同构的「按规范化路径共享静态锁」实现，现共用同一张锁表。</li>
 * </ul>
 *
 * <p><b>共用锁表的影响（有意为之）</b>：此前两类各自持有一张锁表，故同一目录在两类之间**不会**互斥；
 * 合并后会同目录互斥——这是更正确的语义。当前两类操作的是不同目录（{@code face_models} 与
 * {@code face_templates}），故不引入新的争用。
 *
 * <p><b>刻意不改的地方</b>：{@code FeatureTemplateEncryptedStore} 内的 {@code renameTo} 是**裸调用**
 * （不吞 {@code SecurityException}），把它改走 {@link #renameQuietly} 会**改变行为**（异常被吞），
 * 属语义变更而非重构，故不纳入本次抽取。
 */
final class FileStoreUtils {

    /** 按规范化路径共享的静态锁表（进程内）。 */
    private static final ConcurrentHashMap<String, Object> DIR_LOCKS = new ConcurrentHashMap<>();

    private FileStoreUtils() {
    }

    /** 静默重命名；失败返回 false（含 {@code SecurityException}），由调用方决定后续语义。 */
    static boolean renameQuietly(@NonNull File from, @NonNull File to) {
        try {
            return from.renameTo(to);
        } catch (SecurityException ex) {
            return false;
        }
    }

    /**
     * 删除行为接缝（**仅测试注入用**）—— FACE-UI-011 要点⑥ 的「删除失败」用例在纯 JVM 下无法构造：
     * 真实文件系统上很难**稳定**制造 {@code File.delete()} 失败（POSIX 看父目录写权限、Windows 看句柄占用，
     * 两者语义还不一致）。故把 {@link #deleteWithBackup} 用到的**两类**文件系统操作
     * （{@code delete} / {@code exists}，共 **3 处调用**：删 {@code .bak}、删主文件、查主文件存在性）
     * 收敛为可注入接口。
     *
     * <p>与 {@code FaceModelManager.FileOps}、{@code FeatureTemplateEncryptedStore.setPlatformKeyProviderForTesting}
     * 同构（`@VisibleForTesting interface` + `private static final 默认实现` + `private static volatile 字段`
     * + `setXForTesting(null)` 复位）：**默认实现即原有行为，生产路径零变化**。
     */
    @androidx.annotation.VisibleForTesting
    interface FileOps {
        /**
         * 语义同 {@link File#delete()}：返回是否删除成功。
         *
         * <p>⚠️ **可能抛出未受检的 {@link SecurityException}**（与 {@code File#delete()} 一致，
         * 本接缝**不吞**它）。生产侧 {@code AndroidEnrollmentRepository.remove} 正是
         * {@code catch (SecurityException)} 兜住 {@link #deleteWithBackup} 的 —— 别把本方法
         * 读成「从不抛异常」。
         */
        boolean delete(@NonNull File f);

        /** 语义同 {@link File#exists()}。 */
        boolean exists(@NonNull File f);
    }

    private static final FileOps DEFAULT_FILE_OPS = new FileOps() {
        @Override
        public boolean delete(@NonNull File f) {
            return f.delete();
        }

        @Override
        public boolean exists(@NonNull File f) {
            return f.exists();
        }
    };

    private static volatile FileOps fileOps = DEFAULT_FILE_OPS;

    /** 仅供测试注入；传 null 恢复默认（真实文件系统）实现。 */
    @androidx.annotation.VisibleForTesting
    static void setFileOpsForTesting(@Nullable FileOps ops) {
        fileOps = (ops != null) ? ops : DEFAULT_FILE_OPS;
    }

    /**
     * 删除主文件**及其写入器备份**（{@code <name>.bak}）。
     *
     * <p><b>为什么必须一起删</b>：写入器采用「tmp → fsync → dst→bak → tmp→dst → 删 bak」策略，
     * 且读路径有「阶段 1 恢复基线」（主文件缺失时把 {@code .bak} 还原就位）。若删除逻辑只删主文件，
     * 一个陈旧的 {@code .bak} 会在**下次读取时把已删除的数据复活**。
     *
     * <p><b>返回值语义（⚠️ 这是**既有**语义，本次只是补文档，不是设计变更）</b>：以「主文件是否
     * **确实已不存在**」为准而非 {@code delete()} 的返回值 —— 后者在「文件本来就不存在」时也返回
     * false，而那种情况应当算**成功**（幂等删除）。
     *
     * <p>⚠️ <b>异常语义与本类另一工具**不一致**（如实记录，未擅自改行为）</b>：
     * {@link #renameQuietly} 会**吞掉** {@code SecurityException} 并返回 false，而本方法**不吞**、
     * 让它向上传播。两个生产调用方因此行为不同：
     * <ul>
     *   <li>{@code AndroidEnrollmentRepository.remove} —— 有 {@code catch (SecurityException)}，
     *       会回滚注册表并返回 {@code IO_ERROR}（已有用例覆盖，见 {@code AndroidEnrollmentRepositoryTest}）；</li>
     *   <li>{@code PersonDisplayNameStore.remove} —— <b>没有</b> catch，异常会传播并**跳过缓存清理**，
     *       与其 javadoc 承诺的「文件删除失败不影响缓存清理」**相抵触**。</li>
     * </ul>
     * 影响面有限：Android 上 {@code File.delete()} 抛 {@code SecurityException} 需 SecurityManager，
     * 现代 Android 已无 ⇒ 实际不可达。修复方向有二（**待决，本轮只钉现状**）：
     * ① 在调用方 catch；② 让本方法不抛（与 {@link #renameQuietly} 对齐）。
     *
     * @return 主文件是否确实已不存在
     */
    static boolean deleteWithBackup(@NonNull File main) {
        File parent = main.getParentFile();
        if (parent != null) {
            //noinspection ResultOfMethodCallIgnored
            fileOps.delete(new File(parent, main.getName() + ".bak"));
        }
        //noinspection ResultOfMethodCallIgnored
        fileOps.delete(main);
        return !fileOps.exists(main);
    }

    /**
     * 取某目录的共享锁对象：同规范化路径（canonical，失败则退化为 absolute）返回**同一个**锁，
     * 从而串行化同目录的多实例操作。
     */
    @NonNull
    static Object dirLockFor(@NonNull File dir) {
        String key;
        try {
            key = dir.getCanonicalPath();
        } catch (Exception ex) {
            // 规范化失败时退化为绝对路径，仍能覆盖绝大多数同目录场景。
            key = dir.getAbsolutePath();
        }
        Object lock = DIR_LOCKS.get(key);
        if (lock == null) {
            Object fresh = new Object();
            Object prev = DIR_LOCKS.putIfAbsent(key, fresh);
            lock = (prev != null) ? prev : fresh;
        }
        return lock;
    }
}
