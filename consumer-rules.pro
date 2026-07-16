# SPDX-License-Identifier: Apache-2.0
# Aevum consumer ProGuard/R8 rules.
#
# These ship INSIDE the .aar (consumerProguardFiles) and are applied to every app
# that depends on Aevum, so R8/ProGuard in the consuming build cannot break the
# native bridge. They are deliberately narrow: only what JNI, the value-class
# handles, and direct-memory access actually require is kept; everything else in
# the library remains shrinkable/obfuscatable.

# ---------------------------------------------------------------------------
# 1. JNI native-method binding.
# JNI_OnLoad calls FindClass("dev/aevum/AevumNative") and RegisterNatives against
# the exact method table in native/jni/aevum_jni.cpp. If R8 renames the class or
# strips/renames any native method, RegisterNatives fails at load time (the app
# crashes in System.loadLibrary). Keep the class name AND every native method
# signature verbatim.
-keep,includedescriptorclasses class dev.aevum.AevumNative {
    native <methods>;
}
-keepclasseswithmembernames,includedescriptorclasses class dev.aevum.AevumNative {
    native <methods>;
}

# The library loads libaevum.so from a static initializer; keep the holder so the
# initializer (and thus JNI_OnLoad) still runs.
-keep class dev.aevum.AevumNative { *; }

# ---------------------------------------------------------------------------
# 2. Public API surface used by app code and reflection-free type registration.
# Keep the public entry points so a consuming module compiled separately still
# links; members that are purely internal remain shrinkable.
-keep public class dev.aevum.Aevum { public *; }
-keep public class dev.aevum.Aevum$* { public *; }
-keep public class dev.aevum.ManagedHeap { public *; }
-keep public class dev.aevum.ManagedHeap$* { public *; }
-keep public class dev.aevum.AevumException { public *; }

# ---------------------------------------------------------------------------
# 3. Inline value-class handles (Ref / MRef).
# These are @JvmInline value classes wrapping a Long. R8 may unbox them, which is
# fine, but their boxed forms are referenced across the public API boundary; keep
# them and their box/unbox members so a consumer's reflective or cross-module use
# is not mangled. Never strip the underlying Long field.
-keepclassmembers class dev.aevum.Aevum$Ref { *; }
-keepclassmembers class dev.aevum.ManagedHeap$MRef { *; }

# ---------------------------------------------------------------------------
# 4. Direct memory / NIO.
# resolve()/read()/write() hand back a DirectByteBuffer aliasing native arena
# memory. Do not let R8 assume these buffers are unused or elide the calls that
# obtain them (they have native side effects: begin/endWrite bracket a write).
-keepclassmembers class * {
    java.nio.ByteBuffer *(...);
}
-dontwarn java.nio.**

# ---------------------------------------------------------------------------
# 5. Native bridge hygiene.
# Aevum registers all natives via RegisterNatives (not name mangling), so no
# Java_* symbols exist to keep; but keep annotations/signatures R8 might drop that
# JNI descriptor matching depends on.
-keepattributes Signature,Exceptions,InnerClasses,*Annotation*

# The engine spawns no Java threads from native and holds no global refs, so no
# callback keeps are required. If you later add native->JVM callbacks, keep their
# target methods here.
