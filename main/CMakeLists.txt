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
