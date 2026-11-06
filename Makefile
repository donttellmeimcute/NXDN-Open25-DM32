.PHONY: sources toolchain analyze host-test protocol-test codec-test modem-test runtime-test voice-test native-test firstboot-test receive-audio-test radiofoundry target test

sources:
	python3 tools/sources.py fetch

toolchain:
	sh tools/bootstrap_toolchain.sh

analyze:
	python3 tools/analyze_image.py
	python3 experiments/modem/map_known_paths.py

host-test:
	python3 -m unittest discover -s tests -p 'test_*.py' -v

protocol-test:
	python3 tests/protocol/test_p25.py
	sh tests/protocol/run_sanitizers.sh

codec-test:
	python3 experiments/codec/build.py --full-ubsan
	mkdir -p artifacts/codec-tests logs
	experiments/codec/out/ubsan/codec_harness --frames 2000 --fuzz 10000 --out-prefix artifacts/codec-tests/synthetic > logs/codec-tests.json
	experiments/codec/out/ubsan/codec_pipeline artifacts/codec-tests/synthetic.input.s16le artifacts/codec-tests/pipeline > logs/codec-pipeline.json

modem-test:
	python3 -m unittest discover -s experiments/modem -p 'test_*.py' -v

runtime-test:
	mkdir -p build/runtime
	cc -std=c11 -Os -fno-builtin -Dmemcpy=dm32_memcpy -Dmemset=dm32_memset -Dmemmove=dm32_memmove -fsanitize=address,undefined -c src/runtime/memory.c -o build/runtime/memory-host.o
	cc -std=c11 -Os -fsanitize=address,undefined tests/test_memory.c build/runtime/memory-host.o -o build/runtime/test_memory
	build/runtime/test_memory

target:
	python3 tools/build_target.py

voice-test:
	python3 tools/test_voice.py

native-test:
	python3 tools/test_native_modem.py

firstboot-test:
	python3 tools/test_firstboot.py

receive-audio-test:
	python3 tools/test_receive_audio.py

radiofoundry:
	python3 tools/build_radiofoundry.py --stack-check

test: host-test protocol-test codec-test modem-test runtime-test voice-test native-test
