package com.example.rk3288_opencv;

import androidx.annotation.NonNull;

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
     * 删除行为接缝（可注入替身）—— FACE-UI-011 要点⑥ 的「删除失败」用例在纯 JVM 下无法构造：
     * 真实文件系统上很难**稳定**制造 {@code File.delete()} 失败（POSIX 看父目录写权限、Windows 看句柄占用，
     * 两者语义还不一致）。故把 {@link #deleteWithBackup} 用到的**两类**文件系统操作
     * （{@code delete} / {@code exists}，共 **3 处调用**：删 {@code .bak}、删主文件、查主文件存在性）
     * 收敛为可注入接口。
     *
     * <p><b>注入点是实例级，不是进程级静态</b>（子代理 4 P4）：此前本类持有一个
     * {@code private static volatile FileOps fileOps} 全局开关，而 {@code :app:testDebugUnitTest}
     * 的全部测试类在**同一 JVM 内串行**执行，任何测试类注入后忘记复位都会污染其后所有走
     * {@code deleteWithBackup} 的用例。现已改为：{@link #deleteWithBackup(File, FileOps)} 由调用方
     * **显式传入** {@code ops}，两个生产调用方（{@code PersonDisplayNameStore} /
     * {@code AndroidEnrollmentRepository}）各持**实例字段**（默认 {@link #DEFAULT_FILE_OPS}），
     * 并经各自的实例方法 {@code setFileOpsForTesting} 注入 ⇒ 跨实例泄漏在结构上不可能发生。
     *
     * <p><b>默认实现即原有行为，生产路径零变化</b>。
     *
     * <p>⚠️ 本接口**不是** {@code @VisibleForTesting}：注入点改为实例级后，两个生产类各自以
     * {@code private} 字段持有本类型（默认 {@link #DEFAULT_FILE_OPS}），故本类型属**生产内部类型**，
     * 仅**注入方法**（各调用方的 {@code setFileOpsForTesting}）才是测试专用。
     */
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

    /**
     * 真实文件系统实现（{@code File.delete()} / {@code File.exists()}）。生产调用方把它作为
     * 各自的默认 {@link FileOps}（实例字段），测试注入替身时**传 null 即恢复本实现**。
     *
     * <p>包级可见：只给同包的两个生产调用方与单元测试使用；仍是 {@code static final} 不可变常量，
     * 故**不存在**可被篡改的进程级可变状态。
     */
    static final FileOps DEFAULT_FILE_OPS = new FileOps() {
        @Override
        public boolean delete(@NonNull File f) {
            return f.delete();
        }

        @Override
        public boolean exists(@NonNull File f) {
            return f.exists();
        }
    };

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
     * <p><b>异常语义遵循本仓的命名约定（如实记录）</b>：后缀 {@code …Quietly} 表示**吞**异常；
     * **原始操作**表示**不吞**、用返回值报成败（本方法、{@code FaceModelManager.deleteRecursively}）。
     * 二者并存是有意的：需要感知失败（如回滚注册表）的调用方走原始操作，best-effort 收尾走
     * {@code …Quietly}。
     *
     * <p>⚠️ <b>{@code …Quietly} 之间的「捕获宽度」并不一致</b>（A4 补注）：{@link #renameQuietly}
     * **仅**捕 {@code SecurityException}，而 {@code FaceModelManager.deleteRecursivelyQuietly} 捕的是
     * {@code Exception}（连编程错误一并吞）；本方法则**不吞**任何异常。「后缀同名」不等于「吞的范围
     * 相同」，选用/阅读时以各自实现为准。
     *
     * <p><b>本方法不吞 {@code SecurityException}，让它向上传播</b>，由调用方按需包装：
     * <ul>
     *   <li>{@code AndroidEnrollmentRepository.remove} —— {@code catch (SecurityException)}，回滚注册表
     *       并返回 {@code IO_ERROR}（已有用例覆盖，见 {@code AndroidEnrollmentRepositoryTest}）；</li>
     *   <li>{@code PersonDisplayNameStore.remove} —— 在**调用点**自行 catch（该类需保证「文件删除失败
     *       不影响缓存清理」，即 PII 明文擦除优先），是本仓**第二处**「调用点自行包装」的实例
     *       （第一处即上一条 {@code AndroidEnrollmentRepository.remove}）。</li>
     * </ul>
     * Android 上 {@code File.delete()} 抛 {@code SecurityException} 需 SecurityManager，现代 Android
     * 已无 ⇒ 上述包装是**契约护栏**，实际难以触达。
     *
     * @return 主文件是否确实已不存在
     */
    static boolean deleteWithBackup(@NonNull File main, @NonNull FileOps ops) {
        File parent = main.getParentFile();
        if (parent != null) {
            //noinspection ResultOfMethodCallIgnored
            ops.delete(new File(parent, main.getName() + ".bak"));
        }
        //noinspection ResultOfMethodCallIgnored
        ops.delete(main);
        return !ops.exists(main);
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
