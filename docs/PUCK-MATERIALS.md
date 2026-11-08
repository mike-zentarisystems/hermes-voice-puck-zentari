# Puck parts and materials

The puck is a round physical voice companion. The reference firmware targets
**Waveshare ESP32-S3-Touch-LCD-1.85C BOX Rev2**; the browser PWA works without it.
This is a parts overview, not a manufacturing bill of materials or proof that a
new unit or firmware image has passed physical acceptance.

## Reference assembly

| Part | Integrated or supplied? |
| --- | --- |
| ESP32-S3 controller and wireless connectivity | On the board; Wi-Fi and BLE hardware |
| Round 1.85-inch 360×360 touch display | On the board assembly |
| Microphones and audio electronics | Rev2 has dual analog microphones, ES7210 input, ES8311 output and NS4150B amplifier |
| Speaker and enclosure | The vendor lists a speaker in the standard kit; BOX variants include the speaker enclosure. Confirm the selected package |
| USB-C connection and charging circuitry | On the board; cable/power source must be checked separately |
| Battery | Package-dependent; do not assume included |

The [Waveshare documentation](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85C)
distinguishes the standard board, BOX with battery, and BOX without battery.
Confirm the selected package and **Rev2.0** marking before ordering or using it.
V1 and V2 differ in audio hardware and pins; BOX is not a revision identifier.
The repository's [pinned vendor record](../third_party/waveshare/UPSTREAM.md)
and [pin provenance](../hardware/pinmap.json) are the firmware reference, rather
than a similar product name or a changing store listing.

## What you need separately

- **USB data cable and suitable power source**, unless your chosen package
  includes them. Use the vendor's requirements for the exact board revision.
- **Host computer and network access** for the gateway and configured model
  services. The puck is a client, not a self-contained local inference computer.
- **Development setup** for experimental firmware work: ESP-IDF 6.1 and the
  checked-in component lock. See [hardware setup and acceptance](HARDWARE.md).
- **Optional battery**, only if battery operation is wanted and it is absent
  from the package. Confirm compatibility, connector polarity, charging limits
  and fit with the vendor before connecting one. A connector that fits is not
  sufficient evidence of compatibility.

No separate display, microphone board or audio codec is called for by this
reference assembly. Choosing the standard board instead of BOX leaves enclosure
and speaker assembly to be checked; it is not a drop-in shopping list.

## Physical materials and remaining checks

The enclosure's material, finish and manufacturing process are **not confirmed**
in this repository. Battery capacity, cell chemistry, protection and the exact
included pack are also **not confirmed for the selected unit**. This guide does
not specify a replacement pack or claim battery life, price or thermal safety.
Record those details from the actual supplied parts before completing a physical
bill of materials.

This hardware remains experimental. Display/audio bring-up, power and charging,
latency, recovery and soak checks are tracked in the [hardware guide](HARDWARE.md).
A source audit, screenshot or firmware build does not satisfy those checks.
