# ESP32 BLE Rotary Encoder

[![Platform: ESP-IDF](https://img.shields.io/badge/ESP--IDF-v3.0%2B-blue.svg)](https://docs.espressif.com/projects/esp-idf/en/stable/get-started/)

This project makes use of the [esp32-rotary-encoder](https://github.com/DavidAntliff/esp32-rotary-encoder) driver to track the relative position of an [incremental](https://en.wikipedia.org/wiki/Rotary_encoder#Incremental) rotary encoder which is used to deteramine the "zone" of the encoder. This zone is than sent as a BLE notification.

Ensure that submodules are cloned:

    $ git clone --recursive https://github.com/DavidAntliff/esp32-rotary-encoder-example.git

Build the application with:

    $ cd ble_encoder
    $ idf.py menuconfig    # set your serial configuration and the Rotary Encoder GPIO - see Circuit below
    $ idf.py -p (PORT) flash monitor

Install dependencies for the device example:

    $ pip install ./requirements.txt

Run simple device:

    $ python ./device_example.py

## Dependencies

This application makes use of the following components (included as submodules):

 * components/[esp32-rotary-encoder](https://github.com/DavidAntliff/esp32-rotary-encoder)
