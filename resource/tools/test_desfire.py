"""Public DESFire image validation and autonomous native command responses."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HF = ROOT / 'firmware/application/src/rfid/nfctag/hf'


class DesfireTests(unittest.TestCase):
    def test_native_model(self):
        capture = json.loads((Path(__file__).parent / 'tests/desfire-public.json').read_text())
        uid, meta, data = [bytes(capture[k]) for k in ('uid', 'metadata', 'memory')]
        image = bytes([len(uid)]) + uid + len(meta).to_bytes(2, 'big') + len(data).to_bytes(2, 'big') + meta + data
        image += bytes(-len(image) % 4)
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / 'image').write_bytes(image)
            source = directory / 'test.c'
            source.write_text(r'''
#include "desfire_model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    (void)argc;
    FILE *file = fopen(argv[1], "rb"); assert(file);
    uint8_t image[4500], out[32]; size_t n = fread(image, 1, sizeof(image), file); fclose(file);
    desfire_model_t model;
    assert(desfire_image_size(image,n) == n);
    for (size_t i=0;i<n;i++) assert(!desfire_model_load(&model,image,i));
    assert(desfire_model_load(&model,image,n));
    assert(model.app_count == 1 && model.file_count == 3 && model.free_memory == 1536);
    uint8_t version[] = {0x60}, more[] = {0xaf};
    assert(desfire_model_command(&model,version,1,out)==8 && out[0]==0xaf);
    assert(desfire_model_command(&model,more,1,out)==8 && out[0]==0xaf);
    assert(desfire_model_command(&model,more,1,out)==15 && out[0]==0);
    uint8_t select[] = {0x5a,0x33,0x58,0x48};
    assert(desfire_model_command(&model,select,4,out)==1 && out[0]==0);
    uint8_t read[] = {0xbd,1,0,0,0,0,0,0};
    assert(desfire_model_command(&model,read,8,out)==25 && out[0]==0xaf && out[24]==69);
    assert(desfire_model_command(&model,more,1,out)==25 && out[0]==0xaf && out[1]==80);
    assert(desfire_model_command(&model,more,1,out)==17 && out[0]==0 && out[16]==42);
    read[1]=2;
    assert(desfire_model_command(&model,read,8,out)==1 && out[0]==0xf0);
    uint8_t records[] = {0xbb,3,1,0,0,1,0,0};
    assert(desfire_model_command(&model,records,8,out)==3 && out[0]==0 && out[1]==84 && out[2]==217);
    records[2]=4;
    assert(desfire_model_command(&model,records,8,out)==1 && out[0]==0x7e);
    uint8_t auth[] = {0x71,0,0};
    assert(desfire_model_command(&model,auth,3,out)==1 && out[0]==0x1c);
    desfire_model_reset(&model);
    assert(desfire_model_command(&model,more,1,out)==1 && out[0]==0x1c);
    image[0]=255;assert(!desfire_model_load(&model,image,n));
    return 0;
}
''')
            binary = directory / 'test'
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(HF), '-I', str(HF.parent.parent), str(source),
                            str(HF / 'desfire_model.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary), str(directory / 'image')], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
