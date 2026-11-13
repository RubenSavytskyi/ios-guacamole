# adspd serve's boot script (/var/local/adspd.boot): speaker audio at every boot (com.sweet.adspd job).
# The ADSP is up when this runs; see docs/guacamole.md (Audio).
smmu                            # the ADSP's streams (FastRPC, audio buffers) through the identity bank
afe on                          # QUAD_MI2S_IBIT clock, QUAT_MI2S_RX I2S config, DEVICE_START
exec /var/local/bin/tfa init    # both TFA9874 out of powerdown (they need the I2S clock running)
pcm on                          # ASM stream on SweetCodec's ring /var/local/pcm.ring
