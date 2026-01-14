import sys

with open(sys.argv[1], 'rb') as f:
  data = f.read()

"""
Patch for unsigned upgrade tests
It faults the owner_page_validity_check to return kOwnerPageStatusSigned
for unsigned blocks.

   /proc/self/cwd/sw/device/silicon_creator/lib/ownership/ownership.c:54
     if (result != kErrorOk) {
   20009fa2:       8c750513              addi    a0,a0,-1849
>> 20009fa6:   /-- e511                  bnez    a0,20009fb2 <owner_page_validity_check+0xba>
   20009fa8:   |   4e475537              lui     a0,0x4e475
   20009fac:   |   95350513              addi    a0,a0,-1709 # 4e474953 <_build_id_end+0x2e467943>
"""
if True:
  pattern = b"\x11\xe5\x37\x55\x47\x4e"
  target = b"\x2e\x85\x37\x55\x47\x4e"
  print(f'Count: {data.count(pattern)}')
  data = data.replace(pattern, target)


"""
Patch for self_signed upgrade tests
It skips the owner_block_owner_key_equal check.

The search pattern is less reliable; if it is not found, please relocate the
assembly snippet around owner_block_owner_key_equal to update the pattern.

   /proc/self/cwd/sw/device/silicon_creator/lib/ownership/ownership.c:74
         owner_block_owner_key_equal() == kHardenedBoolTrue) {
   20009b46:     955fb0ef       jal     ra,2000549a <owner_block_owner_key_equal>
   /proc/self/cwd/sw/device/silicon_creator/lib/ownership/ownership.c:70
     if (owner_page_valid[0] == kOwnerPageStatusSealed &&
   20009b4a:     8c750513       addi    a0,a0,-1849
>> 20009b4e:     ed0d           bnez    a0,20009b88 <ownership_init+0x1e2>
   /proc/self/cwd/sw/device/silicon_creator/lib/ownership/ownership.c:76
           ownership_activate(bootdata, /*write_both_pages=*/kHardenedBoolFalse);
   20009b50:     1d400593       li      a1,468
   20009b54:     8556           mv      a0,s5
   20009b56:     2b75           jal     2000a112 <ownership_activate>
"""
if False:
  pattern = b"\x13\x05\x75\x8c\x0d\xed\x93\x05\x40\x1d"
  target = b"\x13\x05\x75\x8c\x2e\x85\x93\x05\x40\x1d"
  print(f'Count: {data.count(pattern)}')
  data = data.replace(pattern, target)


with open(sys.argv[2], 'wb') as f:
  f.write(data)
