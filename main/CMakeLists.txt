idf_component_register(
    SRCS "main.cpp"
         "audio/audio_output.cpp"
         "audio/synth.cpp"
         "audio/drums.cpp"
         "dsp/oscillator.cpp"
         "dsp/envelope.cpp"
         "midi/usb_midi.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2s esp_timer
)

# Keep template literals alongside the hot IRAM functions (Xtensa l32r range).
set_source_files_properties(audio/synth.cpp PROPERTIES COMPILE_OPTIONS "-mtext-section-literals")
