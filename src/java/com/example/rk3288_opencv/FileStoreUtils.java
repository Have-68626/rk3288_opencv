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
     * 删除主文件**及其写入器备份**（{@code <name>.bak}）。
     *
     * <p><b>为什么必须一起删</b>：写入器采用「tmp → fsync → dst→bak → tmp→dst → 删 bak」策略，
     * 且读路径有「阶段 1 恢复基线」（主文件缺失时把 {@code .bak} 还原就位）。若删除逻辑只删主文件，
     * 一个陈旧的 {@code .bak} 会在**下次读取时把已删除的数据复活**。
     *
     * @return 主文件是否确实已不存在
     */
    static boolean deleteWithBackup(@NonNull File main) {
        File parent = main.getParentFile();
        if (parent != null) {
            //noinspection ResultOfMethodCallIgnored
            new File(parent, main.getName() + ".bak").delete();
        }
        //noinspection ResultOfMethodCallIgnored
        main.delete();
        return !main.exists();
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
