# globecore 库模块的混淆规则（供消费方 app 打包时应用）
# 保留 JNI 桥接类，防止 native 反查 Kotlin 类/方法失败
-keep class com.zys.globecore.** { *; }
