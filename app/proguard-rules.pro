-dontobfuscate

# for JNI interfacing
-keep class is.xyz.mpv.MPVLib {
	*;
}

-keep interface is.xyz.mpv.MPVLib$Stream {
	*;
}
