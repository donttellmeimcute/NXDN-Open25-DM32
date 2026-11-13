#!/bin/sh
# Fetch exact reference commits into dedicated pristine checkouts.
set -eu
cd "$(dirname "$0")/.."
mkdir -p research/protocol
cd research/protocol
fetch_pin() {
    task_name=$1
    task_url=$2
    task_pin=$3
    if [ ! -d "$task_name" ]; then
        git init "$task_name"
        git -C "$task_name" remote add origin "$task_url"
        git -C "$task_name" fetch --depth 1 origin "$task_pin"
        git -C "$task_name" checkout --detach "$task_pin"
    fi
    test "$(git -C "$task_name" rev-parse HEAD)" = "$task_pin"
    test -z "$(git -C "$task_name" status --porcelain)"
}
fetch_pin op25 https://github.com/boatbod/op25.git 3f44d43eb449bb4753247ce848bb42a403823dea
fetch_pin MMDVM https://github.com/g4klx/MMDVM.git 046e1df16105cbc33de3da7b7d6a03af402ed81d
# sdrtrunk: only its P25 decoder (a sparse checkout of one commit)
if [ ! -d sdrtrunk ]; then
    git init sdrtrunk
    git -C sdrtrunk remote add origin https://github.com/DSheirer/sdrtrunk.git
    git -C sdrtrunk config core.sparseCheckout true
    printf 'src/main/java/io/github/dsheirer/module/decode/p25/\nLICENSE\n' > sdrtrunk/.git/info/sparse-checkout
    git -C sdrtrunk fetch --depth 1 origin 9641d2ef59e40ba89b5453e4c4f052b6502124b5
    git -C sdrtrunk checkout --detach 9641d2ef59e40ba89b5453e4c4f052b6502124b5
fi
test "$(git -C sdrtrunk rev-parse HEAD)" = 9641d2ef59e40ba89b5453e4c4f052b6502124b5
fetch_pdf() {
    task_pdf=$1
    task_url=$2
    task_sha=$3
    if [ ! -f "$task_pdf" ]; then
        curl --fail --location --silent --show-error "$task_url" --output "$task_pdf.download"
        python3 - "$task_pdf.download" "$task_sha" <<'EOF'
import hashlib,sys
assert hashlib.sha256(open(sys.argv[1], 'rb').read()).hexdigest() == sys.argv[2]
EOF
        mv "$task_pdf.download" "$task_pdf"
    fi
    python3 - "$task_pdf" "$task_sha" <<'EOF'
import hashlib,sys
assert hashlib.sha256(open(sys.argv[1], 'rb').read()).hexdigest() == sys.argv[2]
EOF
}
fetch_pdf Zetron-TG-001-5-0-0-P25-Training-Guide.pdf \
    'https://www.zetron.com/wp-content/uploads/2023/12/TG-001-5-0-0-P25-Training-Guide_rebrand.pdf' \
    5b9126a7d3b3d4cc8e6199b64002ddaab4265a79643c64e80ca2d8e1b7ce37e0
fetch_pdf Tait-TRG-00001-01-M-Intro-to-P25.pdf \
    'https://www.taitradioacademy.com/wp-content/uploads/2016/02/TRG-00001-01-M_Intro_to_P25.pdf' \
    9662ea4b323cad04c48bdf3de161d749d61d9a21c39c3baa719f81fd9e2326f5
