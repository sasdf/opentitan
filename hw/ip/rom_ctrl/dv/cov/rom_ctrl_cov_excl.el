// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Manually excluded FSM unreachable states.
//==================================================
// This file contains the Excluded objects
// Generated By User: chencindy
// Format Version: 2
// Date: Fri Feb  4 11:57:07 2022
// ExclMode: default
//==================================================
CHECKSUM: "979204326 3647041052"
INSTANCE: tb.dut.gen_fsm_scramble_enabled.u_checker_fsm
ANNOTATION: "UNR"
Fsm state_d "1410404563"
Transition Checking->RomAhead "341->917"
ANNOTATION: "UNR"
Fsm state_d "1410404563"
Transition KmacAhead->ReadingHigh "629->181"

// In rom_ctrl_scrambled_rom, the wire scr_nonce is having a fixed value and is a part of RHS of
// the continuous assignment on line 81 and 82
INSTANCE: tb.dut.gen_rom_scramble_enabled.u_rom
ANNOTATION: "Waived the continuous assignment block as RHS is a fixed wire"
Block 1 "3862690173" "assign data_scr_nonce = scr_nonce[(63 - Aw):0];"

// In tlul_rsp_intg_gen, the wires on the RHS of continuous assignment at L32 and L43 are having
// constant bits
INSTANCE: tb.dut.u_reg_regs.u_reg_if.u_rsp_intg_gen
ANNOTATION: "Waived the continuous assignment block on L32 and L43 as the RHS is a fixed wire"
Block 1 "461445014" "assign rsp_intg = tl_i.d_user.rsp_intg;"
Block 2 "2643129081" "assign data_intg = tl_i.d_user.data_intg;"

// In u_tl_adapter_rom, data_i is wired to paramter (DataWhenError)
INSTANCE: tb.dut.u_tl_adapter_rom.u_tlul_data_integ_enc_data.u_data_gen
ANNOTATION: "Waived the assignment as the input is wired to a parameter"
Block 1 "3318159292" "data_o = 39'(data_i);"
INSTANCE: tb.dut.u_tl_adapter_rom.u_tlul_data_integ_enc_instr.u_data_gen
Block 1 "3318159292" "data_o = 39'(data_i);"

INSTANCE: tb.dut.gen_rom_scramble_enabled.u_rom.u_rom.u_prim_rom.gen_generic.u_impl_generic
ANNOTATION: "cfg_i is tied to rom_cfg_i in rom_ctrl.sv which is wired to a constant"
Block 1 "118728425" "assign unused_cfg = (^cfg_i);"

INSTANCE: tb.dut.gen_rom_scramble_enabled.u_rom.u_seed_anchor.u_secure_anchor_buf.gen_generic.u_impl_generic
ANNOTATION: "u_seed_anchor inputs are tied to parameters. Therefore this continuous assignment
             cannot see execution"
Block 1 "2421449832" "assign inv = (~in_i);"
Block 2 "1991008127" "assign out_o = (~inv);"

INSTANCE: tb.dut.u_tl_adapter_rom
// wr_attr_error and wr_vld_error both depend on a_opcode != Get. wr_attr_error can exist without
//wr_vld_error but ErrOnWrite is wired to 1 inside the instantiation of tlul_adapter_sram in
//rom_ctrl.sv. Hence, if wr_attr_error is true then tl_i.a_opcode is PutFullData or PutPartialData
//which automatically sets wr_vld_error.
Condition 17 "3469950311" "(wr_attr_error | wr_vld_error | rd_vld_error | instr_error | tlul_error |
                            intg_error) 1 -1" (7 "100000")

// Because en_ifetch_i is hardwired to MuBi4True, instr_error can only be true if
// tl_i.a_user.instr_type is not a valid mubi value. But that gets seen in u_err as instr_type_err,
// causing tlul_error to be true in the adapter.
Condition 17 "3469950311" "(wr_attr_error | wr_vld_error | rd_vld_error | instr_error | tlul_error |
                            intg_error) 1 -1" (4 "000100")

// For rspfifo_rdata.error, it is pushed in rspfifo_wdata.error. If the error field is true, it
// means rerror_i[1] bit is true but the rerror_i bits are wired to 0 in the instantiation of the
// tlul_adapter_sram in rom_ctrl.sv.
Condition 4 "3638058042" "(rspfifo_rdata.error | reqfifo_rdata.error) 1 -1" (3 "10")

// The 011 case is not possible.
//
// Since we are asking that rspfifo_rvalid = 1, we know that if reqfifo_rvalid is true then d_valid
// will be true. As we ask that d_valid = 0, it follows that reqfifo_rvalid is false. This means
// that u_reqfifo is empty.
//
// For rspfifo_rvalid = 1, we must either have a response arriving now (which is visible because
// Pass=1) or have a stored response. In the first case, reqfifo_rvalid must be true (which is
// checked with the rvalidHighReqFifoEmpty assertion). But we know that reqfifo_rvalid must be
// false.
//
// For the second case, we must have had the request in the fifo and popped it before now. But this
// isn't possible, because the ROM always responds in a single cycle: the same cycle we would be
// popping the request from u_reqfifo.
Condition 21 "3623514242" "(d_valid & rspfifo_rvalid & (reqfifo_rdata.op == OpRead)) 1 -1" (1 "011")

// Condition 24 till 34 are excluded for the same reason. The requirements for the conditions are:
// 1) reqfifo_rvalid --> 1
// 2) rspfifo_rvalid --> 1
// 3) reqfifo_rdata.op --> opread
// 4) reqfifo_rdata.error --> 1
//
// We cannot see vld_rd_rsp && d_error. If d_error is true, we must have rspfifo_rdata.error or
// reqfifo_rdata.error. The first cannot happen, because the items in the response fifo have an
// error signal from rerror_i[1] and rom_ctrl connects rerror_i to zero. For the second to happen,
// we must have pushed in an item with error = 1, which means that error_internal was set. But that
// will have squashed the request, so req_o was low. As a result, the request fifo item will be
// popped again before anything appears in the response fifo.
Condition 24 "1059982851" "(vld_rd_rsp & ((~d_error))) 1 -1" (2 "10")

// We cannot see d_valid = 0 and d_error = 1. For d_error to be true, we need reqfifo_rvalid to be
// true. But then the only way for d_valid to be false is if rspfifo_rvalid is false (and
// reqfifo_rdata.op is OpRead).
//
// The ROM always services TL operations without back-pressure and responds in one cycle. As a
// result, after a request is be pushed into u_reqfifo, it is always popped again on the next cycle.
// As a result, if reqfifo_rvalid is true then rspfifo_wvalid will also be true and (because
// u_rspfifo is in pass-through mode) rspfifo_rvalid will be true.
Condition 33 "2509708677" "(d_valid && d_error) 1 -1" (1 "01")

// It is impossible to see rvalid_i && !reqfifo_rvalid. If rvalid_i is true, the ROM is responding
// to a request that was sent on the previous cycle. Since it always responds in exactly one cycle,
// the request will not already have been popped from the request fifo, so reqfifo_rvalid must be
// true.
Condition 43 "721931741" "(rvalid_i & reqfifo_rvalid) 1 -1" (2 "10")

// It is impossible to see !tl_i_int.a_valid & !error_internal. When a_valid is false, it is seen
// in u_err inside tlul_adapter_sram.sv. This will make a_config_allowed inside u_err false as it
// depends on addr_sz_chk, mask_chk and fulldata_chk. All of them are false if a_valid is false.
// This means that in u_err, the first term of err_o becomes true.
// Next, err_o become visible to error_det as tlul_error inside tlul_adapter_sram and it will
// be true if tlul_error is true. error_det will then be seen as error_i to u_sram_byte inside
// tlul_adapter_sram.
// Since EnableIntg parameter is 0 for rom_ctrl, error_i comes out as error_o from u_sram_byte.
// Then error_o would be seen as error_internal to tlul_adapter_sram.
//
// To conclude the reasoning above, it is impossible to see error_internal = 0 when a_valid = 0 as
// when a_valid appears as false to u_err will automatically raise err_o. Then err_o leads to
// error_internal. Thus, we can't expect !tl_i_int.a_valid & !error_internal and for the reasons
// above, the conditional statement is unable to cover case 011.
Condition 36 "413025503" "(tl_i_int.a_valid & reqfifo_wready & ((~error_internal))) 1 -1" (1 "011")

// It is impossible to get sram_ack & we_o. we_o becomes true when there is an a_valid and a_opcode
// as Put. But when a_opcode is a Put, wr_vld_error in the adapter becomes true as ErrOnWrite is
// true for the adapter in case of rom_ctrl. wr_vld_error lets error_det become true. Since
// rom_ctrl doesn't enable integrity errors, error_det comes out as error_internal directly from
// u_sram_byte. But this makes req_o false and hence sram_ack false.
Condition 43 "2041272341" "(sram_ack & ((~we_o))) 1 -1" (2 "10")

// It is impossible to get !sramreqfifo_wready. The depth of sramreqfifo is 2. sramreqfifo_wready
// can be false if the fifo is full. But this can't happen as we don't fill the fifo without
// removing the last item that was pushed in the fifo.
CHECKSUM: "2313518930 2439451700"
INSTANCE: tb.dut.u_tl_adapter_rom
Condition 34 "1999653721" "((gnt_i | missed_err_gnt_q) & reqfifo_wready & sramreqfifo_wready)
                            1 -1" (3 "110")

// The case !empty when under_rst=1 is impossible as fifo clears on reset.
INSTANCE: tb.dut.u_tl_adapter_rom.u_rspfifo
Condition 7 "1709501387" "(((~gen_normal_fifo.empty)) & ((~gen_normal_fifo.under_rst)))
                           1 -1" (2 "10")

INSTANCE: tb.dut
// rom_cfg_i is tied to 0 inside the instantiation of rom_ctrl.
Toggle 0to1 rom_cfg_i.cfg [3:0] "logic rom_cfg_i.cfg[3:0]"
Toggle 1to0 rom_cfg_i.cfg [3:0] "logic rom_cfg_i.cfg[3:0]"
Toggle 0to1 rom_cfg_i.cfg_en "logic rom_cfg_i.cfg_en"
Toggle 1to0 rom_cfg_i.cfg_en "logic rom_cfg_i.cfg_en"
Toggle 0to1 rom_cfg_i.test "logic rom_cfg_i.test"
Toggle 1to0 rom_cfg_i.test "logic rom_cfg_i.test"

// The bits [63:39] of kmac_data_o.data are zero-expanded in rom_ctrl.sv
Toggle 0to1 kmac_data_o.data [63:39] "logic kmac_data_o.data[63:0]"
Toggle 1to0 kmac_data_o.data [63:39] "logic kmac_data_o.data[63:0]"

// kmac_data_o.strb is assigned to 1's in rom_ctrl.sv
Toggle 0to1 kmac_data_o.strb "logic kmac_data_o.strb[7:0]"
Toggle 1to0 kmac_data_o.strb "logic kmac_data_o.strb[7:0]"
