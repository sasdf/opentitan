// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Hardened AND operation for life cycle values. To be used instead of lc_tx_and_*() if the output
// signal must always be rectified (i.e. either ActVal or ~ActVal)/
//

`include "prim_assert.sv"

module prim_lc_and_hardened
  import lc_ctrl_pkg::*;
#(
  lc_tx_t ActVal = On
) (
  // Clock is only used for SVAs in this module.
  input          clk_i,
  input          rst_ni,
  input  lc_tx_t lc_en_a_i,
  input  lc_tx_t lc_en_b_i,
  output lc_tx_t lc_en_o
);
  // We perform strict comparisons and make sure we only output ActVal if both of the two inputs are
  // strictly ActVal. The comparisons are done redundantly so that the multibit aspect is preserved
  // throughout instead of creating a single point of failure due to the reduction.
  lc_tx_t [TxWidth-1:0] lc_en_a_copies;
  prim_lc_sync #(
    .NumCopies(TxWidth),
    .AsyncOn(0) // no sync needed
  ) u_prim_lc_sync_a (
    .clk_i,
    .rst_ni,
    .lc_en_i(lc_en_a_i),
    .lc_en_o(lc_en_a_copies)
  );
  lc_tx_t [TxWidth-1:0] lc_en_b_copies;
  prim_lc_sync #(
    .NumCopies(TxWidth),
    .AsyncOn(0) // no sync needed
  ) u_prim_lc_sync_b (
    .clk_i,
    .rst_ni,
    .lc_en_i(lc_en_b_i),
    .lc_en_o(lc_en_b_copies)
  );

  logic [TxWidth-1:0] lc_en_logic;
  for (genvar k = 0; k < TxWidth; k++) begin : gen_hardened_or
    assign lc_en_logic[k] = (lc_en_a_copies[k] == ActVal) && (lc_en_b_copies[k] == ActVal);
  end
  // So far all comparisons above produce the same value in lc_en_logic.
  // X'oring with the inverse active value will flip the bits that need to be inverted.
  assign lc_en_o = lc_tx_t'(lc_en_logic ^ lc_tx_inv(ActVal));

  ////////////////
  // Assertions //
  ////////////////

  // The outputs should be known at all times.
  `ASSERT_KNOWN(OutputsKnown_A, lc_en_o)
  `ASSERT(FunctionCheck_A, ((lc_en_a_i == ActVal) && (lc_en_b_i == ActVal)) == (lc_en_o == ActVal))

endmodule : prim_lc_and_hardened
