# Power2B Miner for 32-bit ARM (Android Termux)

A CPU miner for Power2B (yespower-based proof-of-work) cryptocurrency, optimized for 32-bit ARM devices running Android Termux.

## Features

- Stratum protocol support for pool mining
- Optimized yespower (Power2B) algorithm with N=2048, r=32, pers="Client Key"
- Targets zpool.ca pool: `stratum+tcp://power2b.eu.mine.zpool.ca:6242`
- Low memory footprint (~8MB per thread)
- Single binary, no external dependencies

## Build

```bash
# Install dependencies
pkg install git make clang

# Clone and build
git clone https://github.com/tundefund0-gif/power2b-miner-arm32.git
cd power2b-miner-arm32
make
```

## Usage

```bash
./power2b-miner -o stratum+tcp://power2b.eu.mine.zpool.ca:6242 \
                -u YOUR_BTC_ADDRESS \
                -p c=BTC
```

With default BTC address (replace with yours):
```bash
./power2b-miner -o stratum+tcp://power2b.eu.mine.zpool.ca:6242 \
                -u 33AUHjnweSG9nz93JJqrsY97EN6o8bNRUC \
                -p c=BTC
```

## ARM Optimized Build

```bash
make arm
```

## Algorithm Details

Power2B uses the yespower proof-of-work with:
- N = 2048 (memory-hardness parameter)
- r = 32 (block size parameter)  
- Personalization string: "Client Key"
- Hash function: BLAKE2b (replacing SHA256 from standard yespower)

## License

Based on the reference implementation from MicroBitcoinOrg/Power2B (BSD-licensed).
