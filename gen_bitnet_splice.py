#!/usr/bin/env python3
"""gen_bitnet_splice.py — Generate spliced HX8K BitNet chain

Generates Verilog for N chips, each containing 6 BitNet 8×8 tiles.
Chips connect via 64-wire (8×8-bit) requantized bus.

Usage:
  python3 gen_bitnet_splice.py [model_weights.json]
  python3 gen_bitnet_splice.py --chips 167   # 167 chips × 6 tiles = 1002 layers
"""

import json, sys, os, math, random

def quantize_bitnet_shift(value):
    """Quantize float to {-1,0,+1} × 2^n → (sign, zero, shift)."""
    if abs(value) < 0.5:
        return (0, 1, 0)
    sign = 1 if value < 0 else 0
    mag = abs(value)
    shift = min(7, max(0, round(math.log2(max(1, mag)))))
    return (sign, 0, shift)

def gen_chip_verilog(chip_idx, layers):
    """Generate Verilog for one HX8K chip containing multiple tile layers.
    
    layers: list of 8×8 weight matrices for tiles inside this chip.
    """
    n_tiles = len(layers)
    
    # Generate per-tile parameter overrides
    tile_insts = []
    for t, weights in enumerate(layers):
        global_layer = chip_idx * 6 + t
        params = []
        for i in range(8):
            for j in range(8):
                w = weights[i][j] if i < len(weights) and j < len(weights[i]) else 0
                s, z, h = quantize_bitnet_shift(w)
                params.append(f"            .S{i}{j}({s}),.Z{i}{j}({z}),.H{i}{j}({h})")
        
        n_neg = sum(1 for i in range(8) for j in range(8)
                    if i < len(weights) and j < len(weights[i])
                    and quantize_bitnet_shift(weights[i][j])[1] == 0
                    and quantize_bitnet_shift(weights[i][j])[0] == 1)
        
        tile_insts.append({
            'idx': t,
            'global_layer': global_layer,
            'params': ",\n".join(params),
            'n_neg': n_neg,
        })
    
    # Estimate LUTs
    total_luts = 0
    tile_blocks = []
    for ti in tile_insts:
        tile_luts = ti['n_neg'] * 16 + 896 + 64  # corrected: 16 LUTs per negate
        total_luts += tile_luts
        tile_blocks.append(f"//   Tile {ti['idx']} (layer {ti['global_layer']}): "
                          f"~{tile_luts} LUTs ({ti['n_neg']} negates)")
    
    routing_luts = 200  # inter-tile requant + IO regs + control
    total_luts += routing_luts
    
    # Build the Verilog with explicit tile instantiations
    # (can't use generate with different parameters, so explicit instances)
    
    wire_decls = []
    inst_code = []
    
    for t_info in tile_insts:
        t = t_info['idx']
        # Stage wires
        if t == 0:
            wire_decls.append(f"    // Input stage")
        wire_decls.append(f"    wire [15:0] s{t}_out [0:7];")
        wire_decls.append(f"    wire s{t}_valid;")
        wire_decls.append(f"    wire [7:0] rq{t} [0:7];")
        
        # Requantize from previous stage
        if t == 0:
            src = "chip_x_in"
            # First tile: input is already 8-bit
            for v in range(8):
                wire_decls.append(f"    assign rq{t}[{v}] = {src}[{v}];")
        else:
            src = f"s{t-1}_out"
            for v in range(8):
                wire_decls.append(
                    f"    assign rq{t}[{v}] = "
                    f"($signed({src}[{v}]) > 16'sd127) ? 8'd127 : "
                    f"($signed({src}[{v}]) < -16'sd128) ? -8'd128 : "
                    f"{src}[{v}][7:0];"
                )
        
        v_in = "chip_valid_in" if t == 0 else f"s{t-1}_valid"
        is_last = (t == n_tiles - 1)
        relu = "1'b0" if is_last else "1'b1"
        
        inst_code.append(f"""    bitnet_tile_8x8 #(
{t_info['params']}
    ) tile_{t} (
        .clk(clk), .rst_n(rst_n),
        .valid_in({v_in}), .x_in(rq{t}),
        .valid_out(s{t}_valid), .y_out(s{t}_out),
        .relu_en({relu})
    );""")
    
    last_t = n_tiles - 1
    
    return f"""// Auto-generated: HX8K chip {chip_idx} — {n_tiles} BitNet tiles
// Layers {chip_idx * 6} to {chip_idx * 6 + n_tiles - 1}
// Estimated LUTs: ~{total_luts} of 7,680 ({total_luts * 100 // 7680}% utilization)
{chr(10).join(tile_blocks)}
//   Routing/IO: ~{routing_luts} LUTs
//   Headroom: {7680 - total_luts} LUTs free

module chip_{chip_idx} (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        chip_valid_in,
    input  wire [7:0]  chip_x_in [0:7],
    output reg         chip_valid_out,
    output reg  [7:0]  chip_y_out [0:7]
);

{chr(10).join(wire_decls)}

{chr(10).join(inst_code)}

    // Output requantize + register for chip-to-chip timing
    integer j;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            chip_valid_out <= 0;
            for (j = 0; j < 8; j = j + 1)
                chip_y_out[j] <= 8'd0;
        end else begin
            chip_valid_out <= s{last_t}_valid;
            for (j = 0; j < 8; j = j + 1) begin
                if ($signed(s{last_t}_out[j]) > 16'sd127)
                    chip_y_out[j] <= 8'd127;
                else if ($signed(s{last_t}_out[j]) < -16'sd128)
                    chip_y_out[j] <= -8'd128;
                else
                    chip_y_out[j] <= s{last_t}_out[j][7:0];
            end
        end
    end

endmodule
"""

def gen_board_top(n_chips):
    """Generate top-level splice connecting all chips."""
    wires = []
    insts = []
    
    for c in range(n_chips):
        if c < n_chips - 1:
            wires.append(f"    wire [7:0] bus_{c+1} [0:7];")
            wires.append(f"    wire bus_{c+1}_valid;")
        
        x_in = "x_in" if c == 0 else f"bus_{c}"
        v_in = "valid_in" if c == 0 else f"bus_{c}_valid"
        y_out = "y_out" if c == n_chips - 1 else f"bus_{c+1}"
        v_out = "valid_out" if c == n_chips - 1 else f"bus_{c+1}_valid"
        
        insts.append(f"""    chip_{c} chip_{c}_inst (
        .clk(clk), .rst_n(rst_n),
        .chip_valid_in({v_in}), .chip_x_in({x_in}),
        .chip_valid_out({v_out}), .chip_y_out({y_out})
    );""")
    
    return f"""// Auto-generated: {n_chips}-chip BitNet splice
// Total tiles: {n_chips * 6}
// Total layers: {n_chips * 6}
// Pipeline latency: {n_chips * 6 + n_chips} clocks (tiles + chip boundaries)
// Throughput: 133M inf/sec (pipelined)
// Cost: {n_chips} × HX8K @ £4 = £{n_chips * 4}

module splice_top (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        valid_in,
    input  wire [7:0]  x_in [0:7],
    output wire        valid_out,
    output wire [7:0]  y_out [0:7]
);

{chr(10).join(wires)}

{chr(10).join(insts)}

endmodule
"""

def main():
    n_chips_override = None
    model_path = None
    
    for arg in sys.argv[1:]:
        if arg.startswith('--chips='):
            n_chips_override = int(arg.split('=')[1])
        elif arg.startswith('--chips'):
            pass  # next arg
        elif sys.argv[sys.argv.index(arg)-1] == '--chips':
            n_chips_override = int(arg)
        elif not arg.startswith('-'):
            model_path = arg
    
    tiles_per_chip = 4  # conservative: ~1,500 LUTs/tile, 4×1500+200 = 6200 (81%)
    
    if model_path:
        with open(model_path) as f:
            model = json.load(f)
        all_layers = [l["weights"] for l in model["layers"]]
    elif n_chips_override:
        total_layers = n_chips_override * tiles_per_chip
        print(f"Generating {n_chips_override} chips × {tiles_per_chip} tiles = {total_layers} layers (random BitNet)")
        random.seed(42)
        all_layers = []
        for _ in range(total_layers):
            w = [[random.choice([-4,-2,-1,0,1,2,4]) for _ in range(8)] for _ in range(8)]
            all_layers.append(w)
    else:
        # Demo: 4 chips = 24 layers
        n_chips_override = 4
        total_layers = n_chips_override * tiles_per_chip
        print(f"Demo: {n_chips_override} chips × {tiles_per_chip} tiles = {total_layers} layers")
        random.seed(42)
        all_layers = []
        for _ in range(total_layers):
            w = [[random.choice([-4,-2,-1,0,1,2,4]) for _ in range(8)] for _ in range(8)]
            all_layers.append(w)
    
    n_chips = (len(all_layers) + tiles_per_chip - 1) // tiles_per_chip
    
    outdir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "rtl", "matmul", "bitnet_splice")
    os.makedirs(outdir, exist_ok=True)
    
    total_luts = 0
    for c in range(n_chips):
        start = c * tiles_per_chip
        end = min(start + tiles_per_chip, len(all_layers))
        chip_layers = all_layers[start:end]
        
        code = gen_chip_verilog(c, chip_layers)
        path = os.path.join(outdir, f"chip_{c}.v")
        with open(path, "w") as f:
            f.write(code)
        
        # Count LUTs
        chip_luts = 0
        for weights in chip_layers:
            n_neg = sum(1 for i in range(8) for j in range(8)
                       if abs(weights[i][j]) >= 0.5 and weights[i][j] < 0)
            chip_luts += n_neg * 16 + 896 + 64
        chip_luts += 200  # routing
        total_luts += chip_luts
        pct = chip_luts * 100 // 7680
        
        status = "OK" if pct < 85 else "TIGHT" if pct < 95 else "OVER"
        print(f"  [{status}] chip_{c}: layers {start}-{end-1}, ~{chip_luts} LUTs ({pct}%)")
    
    # Board top
    top_code = gen_board_top(n_chips)
    path = os.path.join(outdir, "splice_top.v")
    with open(path, "w") as f:
        f.write(top_code)
    print(f"  [OK] splice_top.v")
    
    total_tiles = len(all_layers)
    latency = total_tiles + n_chips  # tile clocks + chip boundary regs
    
    print(f"\n{'='*64}")
    print(f"  BitNet HX8K Splice Chain")
    print(f"{'='*64}")
    print(f"  Chips:           {n_chips} × iCE40HX8K-CT256")
    print(f"  Tiles:           {total_tiles} ({tiles_per_chip} per chip)")
    print(f"  Cost:            {n_chips} × £4 = £{n_chips * 4}")
    print(f"  Inter-chip bus:  64 wires (8 × 8-bit) + 4 control")
    print(f"")
    print(f"  Pipeline latency:  {latency} clocks = {latency / 133:.1f} µs @ 133MHz")
    print(f"  Throughput:        133M inferences/sec (pipelined)")
    print(f"  Ternary MACs/sec:  {total_tiles * 64 * 133 / 1000:.1f} GOPS")
    print(f"  Power:             ~{n_chips * 200}mW ({n_chips} × ~200mW)")
    print(f"")
    print(f"  Per chip: {tiles_per_chip} tiles → ~{tiles_per_chip * 1150} LUTs")
    print(f"  Headroom: ~{7680 - tiles_per_chip * 1150} LUTs for routing/IO")
    print(f"  Utilization: ~{tiles_per_chip * 1150 * 100 // 7680}%")
    print(f"{'='*64}")

if __name__ == "__main__":
    main()
