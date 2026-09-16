package com.example.rk3288_opencv;

import android.content.Context;
import android.os.Build;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Arrays;

import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.Cipher;

public final class FeatureTemplateEncryptedStore {
    public enum Code {
        OK,
        UNSUPPORTED_PLATFORM,
        NOT_FOUND,
        IO_ERROR,
        KEYSTORE_ERROR,
        BAD_FORMAT,
        UNSUPPORTED_VERSION,
        DECRYPT_FAILED,
        CORRUPTED,
        ATOMIC_RENAME_FAILED
    }

    public static final class ReadResult {
        @NonNull public final Code code;
        @Nullable public final byte[] plaintext;

        private ReadResult(@NonNull Code code, @Nullable byte[] plaintext) {
            this.code = code;
            this.plaintext = plaintext;
        }
    }

    public static final class WriteResult {
        @NonNull public final Code code;

        private WriteResult(@NonNull Code code) {
            this.code = code;
        }
    }

    private static final String KEYSTORE = "AndroidKeyStore";
    private static final String KEY_ALIAS_V1 = "feature_aes_gcm_v1";
    /**
     * 显示名用的**独立密钥**（任务 B）：与模板密钥分离，任一用途的密钥轮换不影响另一用途。
     *
     * <p>注意：密钥**刻意不设** {@code setUserAuthenticationRequired(true)}——设了之后密钥只能在
     * 「已设置安全锁屏」的设备上生成（本类设备多为值守机、常无锁屏 → 直接生成失败），且锁屏被
     * 关闭/重配/强制重置会令其**不可逆失效**。默认即 false，保持默认。
     */
    private static final String KEY_ALIAS_NAME_V1 = "person_name_aes_gcm_v1";

    private static final byte[] MAGIC = "FST0".getBytes(StandardCharsets.US_ASCII);
    private static final byte VERSION = 1;
    private static final int IV_LEN = 12;
    private static final int TAG_BITS = 128;

    /**
     * 显示名 AAD 的域分隔标签（任务 B）。
     *
     * <p>与模板 AAD（{@code sha256(userId)||mv||sv}）**不同域**：把模板文件喂给读显示名的路径，
     * 或反之，都会因 GCM 标签校验失败而被拒（{@link Code#DECRYPT_FAILED}），不会静默换用。
     */
    private static final byte[] NAME_AAD_DOMAIN = "NAM0".getBytes(StandardCharsets.US_ASCII);

    /** 显示名单条明文上限（字节）。显示名是短标识，超限一律拒绝而非截断。 */
    private static final int MAX_NAME_BYTES = 256;

    private static final SecureRandom RNG = new SecureRandom();

    /**
     * 平台密钥提供者（测试接缝）。
     *
     * <p>生产实现：API &gt;= 23 时走 AndroidKeyStore（{@link Api23}）。测试可在 JVM 上注入
     * JCE 内存密钥，从而在无 Android 运行时的情况下验证加解密与清零逻辑。
     *
     * <p>红线：AAD 构造与 {@link Api23} 的密钥生成逻辑保持不变，此处仅把“平台是否支持 /
     * 如何取密钥”改为可注入，不改变生产路径行为。
     */
    @androidx.annotation.VisibleForTesting
    interface PlatformKeyProvider {
        boolean isSupported();

        SecretKey getOrCreateKeyV1() throws Exception;

        /**
         * 显示名专用密钥（任务 B）。**默认复用 v1 密钥**——既有测试替身因此无需改动即可获得可用的
         * 加解密能力；生产实现覆盖为独立 alias（见下方 {@code DEFAULT_KEY_PROVIDER}）。
         */
        default SecretKey getOrCreateNameKey() throws Exception {
            return getOrCreateKeyV1();
        }
    }

    private static final PlatformKeyProvider DEFAULT_KEY_PROVIDER = new PlatformKeyProvider() {
        @Override
        public boolean isSupported() {
            return Build.VERSION.SDK_INT >= 23;
        }

        @Override
        public SecretKey getOrCreateKeyV1() throws Exception {
            return Api23.getOrCreateKeyV1();
        }

        @Override
        public SecretKey getOrCreateNameKey() throws Exception {
            return Api23.getOrCreateNameKey();
        }
    };

    private static volatile PlatformKeyProvider platformKeyProvider = DEFAULT_KEY_PROVIDER;

    /** 仅供测试注入；传 null 恢复默认（AndroidKeyStore）实现。 */
    @androidx.annotation.VisibleForTesting
    static void setPlatformKeyProviderForTesting(@Nullable PlatformKeyProvider provider) {
        platformKeyProvider = (provider != null) ? provider : DEFAULT_KEY_PROVIDER;
    }

    /** 当前平台是否支持 AES-GCM 落盘（生产等价于 SDK_INT &gt;= 23）。 */
    @androidx.annotation.VisibleForTesting
    static boolean isPlatformSupported() {
        return platformKeyProvider.isSupported();
    }

    /** 读取回调：明文仅在该回调内有效，回调返回后立即被清零，调用方如需保留必须自行复制。 */
    public interface PlaintextConsumer {
        void accept(@NonNull byte[] plaintextTemplate) throws Exception;
    }

    private FeatureTemplateEncryptedStore() {}

    @NonNull
    public static File defaultDir(@NonNull Context ctx) {
        File dir = new File(ctx.getFilesDir(), "face_templates");
        if (!dir.exists()) dir.mkdirs();
        return dir;
    }

    @NonNull
    public static File fileForUserId(@NonNull Context ctx, @NonNull String userId) {
        return fileForUserIdInDir(defaultDir(ctx), userId);
    }

    /**
     * 通用作用域版本：在调用方指定的目录内计算某 userId 的模板文件名。
     * 命名语义与 {@link #fileForUserId(Context, String)} 完全一致（userId 的 SHA-256 十六进制）。
     */
    @NonNull
    public static File fileForUserIdInDir(@NonNull File dir, @NonNull String userId) {
        String name = "u_" + toHex(sha256(userId.getBytes(StandardCharsets.UTF_8))) + ".bin";
        return new File(dir, name);
    }

    // ── 显示名加密存储（任务 B）────────────────────────────────────────────────
    // 与模板**同款信封与落盘语义**，但用**独立密钥**（KEY_ALIAS_NAME_V1）与**独立 AAD 域**
    // （NAME_AAD_DOMAIN），且文件名前缀不同（n_ vs u_）——避免与 AndroidEnrollmentRepository.add
    // 的「模板已存在」判重相撞。API<23 与密钥不可用时的语义与模板一致（返回错误码、不抛异常）。

    /**
     * 显示名文件路径：{@code n_<sha256(personId)>.enc}。
     *
     * <p>文件名由 personId 单向派生（不落 personId 明文），且与模板的 {@code u_*.bin} 前缀区分。
     */
    @NonNull
    public static File nameFileForPersonInDir(@NonNull File dir, @NonNull String personId) {
        return new File(dir, "n_" + toHex(sha256(personId.getBytes(StandardCharsets.UTF_8))) + ".enc");
    }

    /** 显示名 AAD：域标签 ‖ sha256(personId)。与模板 AAD 不同域，跨用途换用会被 GCM 标签拒绝。 */
    private static byte[] buildNameAad(@NonNull String personId) {
        byte[] idDigest = sha256(personId.getBytes(StandardCharsets.UTF_8));
        byte[] aad = new byte[NAME_AAD_DOMAIN.length + idDigest.length];
        System.arraycopy(NAME_AAD_DOMAIN, 0, aad, 0, NAME_AAD_DOMAIN.length);
        System.arraycopy(idDigest, 0, aad, NAME_AAD_DOMAIN.length, idDigest.length);
        return aad;
    }

    /**
     * 取 Keystore 生成的 GCM IV（加密路径必须走这里，不得自造 IV）。
     *
     * <p><b>为什么不能自己造 IV</b>：密钥以 {@code setRandomizedEncryptionRequired(true)} 生成，
     * AndroidKeyStore 在这种情况下会**拒绝调用方提供的 IV**——加密时抛
     * {@code InvalidAlgorithmParameterException: Caller-provided IV not permitted}。必须让 Keystore
     * 自行生成、再经 {@link Cipher#getIV()} 回读并随密文落盘（解密侧仍用信封里的该 IV）。
     *
     * <p>⚠️ **JVM 的 JCE 替身没有这条限制**，故单元测试无法覆盖该约束——模板写入与显示名写入
     * 这两条路径**必须在真机验证**（本项目尚无实机留档）。
     */
    @NonNull
    private static byte[] requireIv(@NonNull Cipher cipher) {
        byte[] iv = cipher.getIV();
        if (iv == null || iv.length == 0) {
            throw new IllegalStateException("KEYSTORE_ERROR: 加密未产生 IV");
        }
        return iv;
    }

    /**
     * 加密写入一条显示名。明文为 {@code displayName} 的 UTF-8 字节，写后立即清零。
     *
     * @return {@link Code#BAD_FORMAT} = 显示名为空或超过 {@link #MAX_NAME_BYTES} 字节
     *         （**拒绝而非截断**）；{@link Code#UNSUPPORTED_PLATFORM} = 平台不支持（API&lt;23）。
     */
    @NonNull
    public static WriteResult writeNameString(@NonNull File dstFile,
                                              @NonNull String personId,
                                              @NonNull String displayName) {
        byte[] plain = displayName.getBytes(StandardCharsets.UTF_8);
        try {
            if (plain.length == 0 || plain.length > MAX_NAME_BYTES) {
                return new WriteResult(Code.BAD_FORMAT);
            }
            if (!platformKeyProvider.isSupported()) {
                return new WriteResult(Code.UNSUPPORTED_PLATFORM);
            }

            SecretKey key = platformKeyProvider.getOrCreateNameKey();
            byte[] aad = buildNameAad(personId);

            Cipher cipher;
            try {
                cipher = Cipher.getInstance("AES/GCM/NoPadding");
            } catch (java.security.NoSuchAlgorithmException | javax.crypto.NoSuchPaddingException e) {
                throw new IllegalStateException("KEYSTORE_ERROR: AES/GCM/NoPadding not found", e);
            }
            // 与模板路径同理：IV 由 Keystore 生成并回读，**不得**传调用方 IV。
            cipher.init(Cipher.ENCRYPT_MODE, key);
            byte[] iv = requireIv(cipher);
            cipher.updateAAD(aad);
            byte[] ciphertextAndTag = cipher.doFinal(plain);

            return writeEnvelopeAtomically(dstFile, encodeEnvelope(iv, aad, ciphertextAndTag));
        } catch (Exception e) {
            return new WriteResult(mapWriteError(e));
        } finally {
            Arrays.fill(plain, (byte) 0);
        }
    }

    /**
     * 低层读取显示名：返回的 {@link ReadResult#plaintext} 是 UTF-8 显示名字节，**由调用方使用后清零**。
     *
     * @param quarantineOnCorrupt 是否把损坏文件重命名为 {@code *.corrupt.<ts>}；显示名损坏可降级为
     *                            personId，通常传 false 即可（避免在显示名目录堆积隔离文件）。
     */
    @NonNull
    public static ReadResult readNameString(@NonNull File srcFile,
                                            @NonNull String personId,
                                            boolean quarantineOnCorrupt) {
        if (!platformKeyProvider.isSupported()) {
            return new ReadResult(Code.UNSUPPORTED_PLATFORM, null);
        }
        return readEnvelope(srcFile,
                () -> platformKeyProvider.getOrCreateNameKey(),
                () -> buildNameAad(personId),
                quarantineOnCorrupt);
    }

    @NonNull
    public static WriteResult write(@NonNull Context ctx,
                                    @NonNull File dstFile,
                                    @NonNull String userId,
                                    @NonNull byte[] plaintextTemplate,
                                    int modelVersion,
                                    int templateSchemaVersion) {
        if (!platformKeyProvider.isSupported()) {
            return new WriteResult(Code.UNSUPPORTED_PLATFORM);
        }

        try {
            SecretKey key = platformKeyProvider.getOrCreateKeyV1();
            byte[] aad = buildAad(userId, modelVersion, templateSchemaVersion);
            Cipher cipher;
            try {
                cipher = Cipher.getInstance("AES/GCM/NoPadding");
            } catch (java.security.NoSuchAlgorithmException | javax.crypto.NoSuchPaddingException e) {
                throw new IllegalStateException("KEYSTORE_ERROR: AES/GCM/NoPadding not found", e);
            }
            // IV 必须由 Keystore 生成并回读（见 newGcmCipher 的注释）：密钥以
            // setRandomizedEncryptionRequired(true) 生成，AndroidKeyStore 会**拒绝调用方提供的 IV**。
            cipher.init(Cipher.ENCRYPT_MODE, key);
            byte[] iv = requireIv(cipher);
            cipher.updateAAD(aad);
            byte[] ciphertextAndTag = cipher.doFinal(plaintextTemplate);

            byte[] envelope = encodeEnvelope(iv, aad, ciphertextAndTag);

            return writeEnvelopeAtomically(dstFile, envelope);
        } catch (Exception e) {
            return new WriteResult(mapWriteError(e));
        }
    }

    /**
     * 把已编码信封原子落盘：父目录 → 临时文件 + flush + fsync → dst→bak → tmp→dst → 删 bak。
     *
     * <p>由模板写入与显示名写入共用（同一份落盘语义）。异常交由调用方按各自错误映射处理。
     */
    @NonNull
    private static WriteResult writeEnvelopeAtomically(@NonNull File dstFile, @NonNull byte[] envelope)
            throws Exception {
        File parent = dstFile.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) {
            return new WriteResult(Code.IO_ERROR);
        }

        String tmpName = dstFile.getName() + ".tmp." + Long.toHexString(RNG.nextLong());
        File tmpFile = new File(parent != null ? parent : dstFile.getAbsoluteFile().getParentFile(), tmpName);

        try (FileOutputStream fos = new FileOutputStream(tmpFile, false)) {
            fos.write(envelope);
            fos.flush();
            try {
                fos.getFD().sync();
            } catch (Exception ignored) {
            }
        }

        File bakFile = new File(tmpFile.getParentFile(), dstFile.getName() + ".bak");
        if (bakFile.exists()) bakFile.delete();

        if (dstFile.exists()) {
            if (!dstFile.renameTo(bakFile)) {
                tmpFile.delete();
                return new WriteResult(Code.ATOMIC_RENAME_FAILED);
            }
        }

        boolean moved = tmpFile.renameTo(dstFile);
        if (!moved) {
            if (bakFile.exists()) bakFile.renameTo(dstFile);
            tmpFile.delete();
            return new WriteResult(Code.ATOMIC_RENAME_FAILED);
        }

        if (bakFile.exists()) bakFile.delete();
        return new WriteResult(Code.OK);
    }

    /**
     * 低层读取原语：返回的 {@link ReadResult#plaintext} 由调用方负责在使用后清零。
     * 生产代码请使用 {@link #readAndConsume}，由库保证使用后清零，避免明文长期驻留。
     */
    @NonNull
    public static ReadResult read(@NonNull Context ctx,
                                  @NonNull File srcFile,
                                  @NonNull String userId,
                                  int modelVersion,
                                  int templateSchemaVersion,
                                  boolean quarantineOnCorrupt) {
        if (!platformKeyProvider.isSupported()) {
            return new ReadResult(Code.UNSUPPORTED_PLATFORM, null);
        }
        return readEnvelope(srcFile,
                () -> platformKeyProvider.getOrCreateKeyV1(),
                () -> buildAad(userId, modelVersion, templateSchemaVersion),
                quarantineOnCorrupt);
    }

    /** 取密钥的延迟求值口：保证「密钥获取」发生在文件读取与信封解码**之后**（与原实现顺序一致）。 */
    @FunctionalInterface
    private interface KeySupplier {
        SecretKey get() throws Exception;
    }

    /**
     * 期望 AAD 的延迟求值口（任务 B 修复 A3 确认的边界差异）：AAD 必须在 **try 内**求值。
     *
     * <p>提取时曾把 {@code buildAad(...)} 作为实参在调用前求值，导致 {@code userId == null} 时
     * 抛 NPE 穿透，而原实现会把它映射为错误码。改为延迟求值后与提取前一致。
     */
    @FunctionalInterface
    private interface AadSupplier {
        byte[] get();
    }

    /**
     * 读文件 → 解信封 → 校验 AAD → 解密。模板与显示名共用（同一份落盘语义）。
     *
     * <p>顺序刻意与提取前一致：{@code isSupported} 由调用方前置判定 → 文件存在性 → 读文件 →
     * 解信封 → **此时才取密钥** → AAD 比对 → 解密。密钥获取失败与解密失败同归 {@link #mapReadError}。
     * 若为「密钥不可用且文件不存在」，仍应报 `NOT_FOUND` 而非 `KEYSTORE_ERROR`。
     */
    @NonNull
    private static ReadResult readEnvelope(@NonNull File srcFile,
                                           @NonNull KeySupplier keySupplier,
                                           @NonNull AadSupplier aadSupplier,
                                           boolean quarantineOnCorrupt) {
        if (!srcFile.exists()) {
            return new ReadResult(Code.NOT_FOUND, null);
        }

        byte[] raw;
        try (FileInputStream fis = new FileInputStream(srcFile)) {
            raw = readAllBytesCompat(fis);
        } catch (Exception e) {
            return new ReadResult(Code.IO_ERROR, null);
        }

        Envelope env;
        try {
            env = decodeEnvelope(raw);
        } catch (BadEnvelopeException e) {
            if (quarantineOnCorrupt) quarantine(srcFile);
            return new ReadResult(e.code, null);
        } catch (Exception e) {
            if (quarantineOnCorrupt) quarantine(srcFile);
            return new ReadResult(Code.CORRUPTED, null);
        }

        try {
            SecretKey key = keySupplier.get();
            // 与原实现一致：AAD 在 try 内求值，故 null userId 之类会归入下面的错误映射，
            // 而不是作为实参提前求值抛 NPE 穿透（A3 确认的提取边界差异）。
            byte[] expectedAad = aadSupplier.get();
            if (!Arrays.equals(env.aad, expectedAad)) {
                if (quarantineOnCorrupt) quarantine(srcFile);
                return new ReadResult(Code.CORRUPTED, null);
            }

            Cipher cipher;
            try {
                cipher = Cipher.getInstance("AES/GCM/NoPadding");
            } catch (java.security.NoSuchAlgorithmException | javax.crypto.NoSuchPaddingException e) {
                throw new IllegalStateException("KEYSTORE_ERROR: AES/GCM/NoPadding not found", e);
            }
            cipher.init(Cipher.DECRYPT_MODE, key, new GCMParameterSpec(TAG_BITS, env.iv));
            cipher.updateAAD(env.aad);
            byte[] pt = cipher.doFinal(env.ciphertextAndTag);
            return new ReadResult(Code.OK, pt);
        } catch (javax.crypto.AEADBadTagException e) {
            if (quarantineOnCorrupt) quarantine(srcFile);
            return new ReadResult(Code.CORRUPTED, null);
        } catch (Exception e) {
            return new ReadResult(mapReadError(e), null);
        }
    }

    /**
     * 读取模板并在使用后清零内存明文（推荐的生产入口）。
     *
     * <p>与 {@link #read} 的差异：明文不对外返回，而是交给 {@code consumer} 使用；无论回调
     * 正常返回还是抛异常，明文缓冲区都会在 finally 中 {@code Arrays.fill(pt, (byte) 0)}。
     * 这样调用方无法持有长期存活的明文副本，避免模板残留在堆内存。
     *
     * <p>注意：本方法复用 {@link #read} 的全部加解密路径，AAD 与 Keystore 逻辑不受影响。
     *
     * @return {@code OK} 表示回调已成功执行且明文已清零；其余 code 与 {@link #read} 一致。
     */
    @NonNull
    public static ReadResult readAndConsume(@NonNull Context ctx,
                                            @NonNull File srcFile,
                                            @NonNull String userId,
                                            int modelVersion,
                                            int templateSchemaVersion,
                                            boolean quarantineOnCorrupt,
                                            @NonNull PlaintextConsumer consumer) {
        ReadResult rr = read(ctx, srcFile, userId, modelVersion, templateSchemaVersion, quarantineOnCorrupt);
        if (rr.code != Code.OK || rr.plaintext == null) {
            return new ReadResult(rr.code, null);
        }
        try {
            consumer.accept(rr.plaintext);
            return new ReadResult(Code.OK, null);
        } catch (Exception e) {
            // 回调失败不回传明文，避免泄漏；错误归为解密/消费失败。
            return new ReadResult(Code.DECRYPT_FAILED, null);
        } finally {
            // 明文仅存在于本次调用栈内，使用后立即清零。
            Arrays.fill(rr.plaintext, (byte) 0);
        }
    }

    private static void quarantine(@NonNull File f) {
        File dir = f.getParentFile();
        if (dir == null) return;
        String name = f.getName() + ".corrupt." + System.currentTimeMillis();
        File dst = new File(dir, name);
        f.renameTo(dst);
    }

    private static Code mapWriteError(@NonNull Exception e) {
        if (e instanceof IllegalStateException) return Code.KEYSTORE_ERROR;
        String cn = e.getClass().getName();
        if (cn.contains("KeyStore") || cn.contains("android.security")) return Code.KEYSTORE_ERROR;
        if (cn.contains("InvalidAlgorithmParameter") || cn.contains("InvalidKey")) return Code.KEYSTORE_ERROR;
        return Code.IO_ERROR;
    }

    private static Code mapReadError(@NonNull Exception e) {
        if (e instanceof IllegalStateException) return Code.KEYSTORE_ERROR;
        String cn = e.getClass().getName();
        if (cn.contains("KeyStore") || cn.contains("android.security")) return Code.KEYSTORE_ERROR;
        if (cn.contains("InvalidAlgorithmParameter") || cn.contains("InvalidKey")) return Code.KEYSTORE_ERROR;
        return Code.DECRYPT_FAILED;
    }

    private static final class Api23 {
        private Api23() {}

        @android.annotation.TargetApi(23)
        private static SecretKey getOrCreateKeyV1() throws Exception {
            return getOrCreate(KEY_ALIAS_V1);
        }

        /** 显示名专用密钥（任务 B）：独立 alias，轮换互不影响。 */
        @android.annotation.TargetApi(23)
        private static SecretKey getOrCreateNameKey() throws Exception {
            return getOrCreate(KEY_ALIAS_NAME_V1);
        }

        @android.annotation.TargetApi(23)
        private static SecretKey getOrCreate(String alias) throws Exception {
            KeyStore ks;
            try {
                ks = KeyStore.getInstance(KEYSTORE);
            } catch (java.security.KeyStoreException e) {
                throw new IllegalStateException("KEYSTORE_ERROR: KeyStore not found", e);
            }
            ks.load(null);

            java.security.Key existing = ks.getKey(alias, null);
            if (existing instanceof SecretKey) return (SecretKey) existing;

            javax.crypto.KeyGenerator kg;
            try {
                kg = javax.crypto.KeyGenerator.getInstance(android.security.keystore.KeyProperties.KEY_ALGORITHM_AES, KEYSTORE);
            } catch (java.security.NoSuchAlgorithmException | java.security.NoSuchProviderException e) {
                throw new IllegalStateException("KEYSTORE_ERROR: AES KeyGenerator not found", e);
            }
            android.security.keystore.KeyGenParameterSpec.Builder b = new android.security.keystore.KeyGenParameterSpec.Builder(
                    alias,
                    android.security.keystore.KeyProperties.PURPOSE_ENCRYPT | android.security.keystore.KeyProperties.PURPOSE_DECRYPT
            );
            b.setBlockModes(android.security.keystore.KeyProperties.BLOCK_MODE_GCM);
            b.setEncryptionPaddings(android.security.keystore.KeyProperties.ENCRYPTION_PADDING_NONE);
            b.setRandomizedEncryptionRequired(true);
            try {
                b.setKeySize(256);
            } catch (Exception ignored) {
            }
            try {
                kg.init(b.build());
                return kg.generateKey();
            } catch (Exception e) {
                try {
                    b.setKeySize(128);
                    kg.init(b.build());
                    return kg.generateKey();
                } catch (Exception e2) {
                    throw e;
                }
            }
        }
    }

    private static final class Envelope {
        final byte[] iv;
        final byte[] aad;
        final byte[] ciphertextAndTag;

        private Envelope(byte[] iv, byte[] aad, byte[] ciphertextAndTag) {
            this.iv = iv;
            this.aad = aad;
            this.ciphertextAndTag = ciphertextAndTag;
        }
    }

    private static final class BadEnvelopeException extends Exception {
        final Code code;

        private BadEnvelopeException(Code code) {
            this.code = code;
        }
    }

    private static byte[] encodeEnvelope(@NonNull byte[] iv, @NonNull byte[] aad, @NonNull byte[] ciphertextAndTag) {
        int len = 4 + 1 + 1 + iv.length + 2 + aad.length + 4 + ciphertextAndTag.length;
        ByteArrayOutputStream out = new ByteArrayOutputStream(len);
        out.write(MAGIC, 0, MAGIC.length);
        out.write(VERSION);
        out.write(iv.length & 0xFF);
        out.write(iv, 0, iv.length);
        out.write((aad.length >>> 8) & 0xFF);
        out.write((aad.length) & 0xFF);
        out.write(aad, 0, aad.length);
        out.write((ciphertextAndTag.length >>> 24) & 0xFF);
        out.write((ciphertextAndTag.length >>> 16) & 0xFF);
        out.write((ciphertextAndTag.length >>> 8) & 0xFF);
        out.write((ciphertextAndTag.length) & 0xFF);
        out.write(ciphertextAndTag, 0, ciphertextAndTag.length);
        return out.toByteArray();
    }

    private static Envelope decodeEnvelope(@NonNull byte[] input) throws BadEnvelopeException {
        if (input.length < 4 + 1 + 1 + IV_LEN + 2 + 4) throw new BadEnvelopeException(Code.BAD_FORMAT);

        int i = 0;
        byte[] magic = Arrays.copyOfRange(input, i, i + 4);
        i += 4;
        if (!Arrays.equals(magic, MAGIC)) throw new BadEnvelopeException(Code.BAD_FORMAT);

        byte ver = input[i++];
        if (ver != VERSION) throw new BadEnvelopeException(Code.UNSUPPORTED_VERSION);

        int ivLen = input[i++] & 0xFF;
        if (ivLen != IV_LEN) throw new BadEnvelopeException(Code.BAD_FORMAT);
        if (i + ivLen > input.length) throw new BadEnvelopeException(Code.BAD_FORMAT);
        byte[] iv = Arrays.copyOfRange(input, i, i + ivLen);
        i += ivLen;

        if (i + 2 > input.length) throw new BadEnvelopeException(Code.BAD_FORMAT);
        int aadLen = ((input[i] & 0xFF) << 8) | (input[i + 1] & 0xFF);
        i += 2;
        if (i + aadLen > input.length) throw new BadEnvelopeException(Code.BAD_FORMAT);
        byte[] aad = Arrays.copyOfRange(input, i, i + aadLen);
        i += aadLen;

        if (i + 4 > input.length) throw new BadEnvelopeException(Code.BAD_FORMAT);
        int ctLen = ((input[i] & 0xFF) << 24) | ((input[i + 1] & 0xFF) << 16) | ((input[i + 2] & 0xFF) << 8) | (input[i + 3] & 0xFF);
        i += 4;
        if (ctLen <= 0) throw new BadEnvelopeException(Code.BAD_FORMAT);
        if (i + ctLen > input.length) throw new BadEnvelopeException(Code.BAD_FORMAT);
        byte[] ct = Arrays.copyOfRange(input, i, i + ctLen);
        return new Envelope(iv, aad, ct);
    }

    private static byte[] buildAad(@NonNull String userId, int modelVersion, int templateSchemaVersion) {
        byte[] uid = sha256(userId.getBytes(StandardCharsets.UTF_8));
        byte[] aad = new byte[uid.length + 4 + 4];
        System.arraycopy(uid, 0, aad, 0, uid.length);
        putU32BE(aad, uid.length, modelVersion);
        putU32BE(aad, uid.length + 4, templateSchemaVersion);
        return aad;
    }

    private static void putU32BE(@NonNull byte[] out, int off, int v) {
        out[off] = (byte) ((v >>> 24) & 0xFF);
        out[off + 1] = (byte) ((v >>> 16) & 0xFF);
        out[off + 2] = (byte) ((v >>> 8) & 0xFF);
        out[off + 3] = (byte) ((v) & 0xFF);
    }

    @androidx.annotation.VisibleForTesting
    static byte[] sha256(byte[] input) {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            return md.digest(input);
        } catch (java.security.NoSuchAlgorithmException e) {
            throw new IllegalStateException("KEYSTORE_ERROR: SHA-256 algorithm not available", e);
        }
    }

    /** SHA-256 十六进制摘要（小写）。用于登记密文完整性来源，不涉及模板明文。 */
    @NonNull
    public static String sha256Hex(@NonNull byte[] input) {
        return toHex(sha256(input));
    }

    private static String toHex(@NonNull byte[] bytes) {
        char[] HEX = "0123456789abcdef".toCharArray();
        char[] out = new char[bytes.length * 2];
        for (int i = 0; i < bytes.length; i++) {
            int v = bytes[i] & 0xFF;
            out[i * 2] = HEX[v >>> 4];
            out[i * 2 + 1] = HEX[v & 0x0F];
        }
        return new String(out);
    }

    private static byte[] readAllBytesCompat(@NonNull FileInputStream in) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) >= 0) {
            if (n == 0) continue;
            out.write(buf, 0, n);
        }
        return out.toByteArray();
    }
}
