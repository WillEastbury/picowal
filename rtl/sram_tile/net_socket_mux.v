// net_socket_mux.v -- Layer 5: Socket multiplexer
// Routes TCP connections to protocol engines by destination port
//
// Stack: SerDes -> MAC -> IP -> TCP -> [THIS] -> proto_smb2 / proto_http / pico_bus
//
// Port routing (configurable):
//   445  -> SMB2/CIFS engine (proto_smb2)
//   80   -> HTTP engine (proto_http)
//   443  -> HTTPS/TLS engine (proto_tls -> proto_http)
//   5432 -> DB wire protocol (pico bus -> session pico)
//   *    -> default: pico bus (custom app)
//
`default_nettype none

module net_socket_mux #(
    parameter N_ENGINES = 4   // number of protocol engines
)(
    input  wire        clk,
    input  wire        rst_n,

    // From TCP engine (socket stream)
    input  wire [7:0]  sock_rx_data,
    input  wire        sock_rx_valid,
    input  wire        sock_rx_sof,
    input  wire        sock_rx_eof,
    input  wire [5:0]  sock_rx_id,
    input  wire [15:0] sock_rx_port,
    output wire        sock_rx_ready,

    // To TCP engine (response stream)
    output reg  [7:0]  sock_tx_data,
    output reg         sock_tx_valid,
    output reg         sock_tx_sof,
    output reg         sock_tx_eof,
    output reg  [5:0]  sock_tx_id,
    input  wire        sock_tx_ready,

    // Engine 0: SMB2 (port 445)
    output reg  [7:0]  smb_rx_data,
    output reg         smb_rx_valid,
    output reg         smb_rx_sof,
    output reg         smb_rx_eof,
    output reg  [5:0]  smb_rx_conn,
    input  wire [7:0]  smb_tx_data,
    input  wire        smb_tx_valid,
    input  wire        smb_tx_sof,
    input  wire        smb_tx_eof,
    input  wire [5:0]  smb_tx_conn,
    output wire        smb_tx_ready,

    // Engine 1: HTTP (port 80)
    output reg  [7:0]  http_rx_data,
    output reg         http_rx_valid,
    output reg         http_rx_sof,
    output reg         http_rx_eof,
    output reg  [5:0]  http_rx_conn,
    input  wire [7:0]  http_tx_data,
    input  wire        http_tx_valid,
    input  wire        http_tx_sof,
    input  wire        http_tx_eof,
    input  wire [5:0]  http_tx_conn,
    output wire        http_tx_ready,

    // Engine 2: DB protocol (port 5432)
    output reg  [7:0]  db_rx_data,
    output reg         db_rx_valid,
    output reg         db_rx_sof,
    output reg         db_rx_eof,
    output reg  [5:0]  db_rx_conn,
    input  wire [7:0]  db_tx_data,
    input  wire        db_tx_valid,
    input  wire        db_tx_sof,
    input  wire        db_tx_eof,
    input  wire [5:0]  db_tx_conn,
    output wire        db_tx_ready,

    // Engine 3: Pico passthrough (any other port)
    output reg  [7:0]  pico_rx_data,
    output reg         pico_rx_valid,
    output reg         pico_rx_sof,
    output reg         pico_rx_eof,
    output reg  [5:0]  pico_rx_conn,
    input  wire [7:0]  pico_tx_data,
    input  wire        pico_tx_valid,
    input  wire        pico_tx_sof,
    input  wire        pico_tx_eof,
    input  wire [5:0]  pico_tx_conn,
    output wire        pico_tx_ready
);

    // ── Connection-to-engine mapping table ──
    // Once a connection arrives on a port, we lock it to an engine
    reg [1:0] conn_engine [0:63];  // 0=SMB, 1=HTTP, 2=DB, 3=PICO
    reg       conn_mapped [0:63];

    integer i;

    // Port-to-engine mapping (combinational)
    reg [1:0] port_engine;
    always @(*) begin
        case (sock_rx_port)
            16'd445:  port_engine = 2'd0;  // SMB2/CIFS
            16'd80:   port_engine = 2'd1;  // HTTP
            16'd8080: port_engine = 2'd1;  // HTTP alt
            16'd5432: port_engine = 2'd2;  // DB wire protocol
            default:  port_engine = 2'd3;  // Pico passthrough
        endcase
    end

    assign sock_rx_ready = 1'b1;

    // ── RX demux: route incoming data to correct engine ──
    wire [1:0] active_engine = conn_mapped[sock_rx_id] ?
                               conn_engine[sock_rx_id] : port_engine;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            smb_rx_valid  <= 0; http_rx_valid  <= 0;
            db_rx_valid   <= 0; pico_rx_valid  <= 0;
            for (i = 0; i < 64; i = i + 1)
                conn_mapped[i] <= 0;
        end else begin
            smb_rx_valid  <= 0;
            http_rx_valid <= 0;
            db_rx_valid   <= 0;
            pico_rx_valid <= 0;

            if (sock_rx_valid) begin
                // Lock connection to engine on first packet
                if (!conn_mapped[sock_rx_id]) begin
                    conn_engine[sock_rx_id] <= port_engine;
                    conn_mapped[sock_rx_id] <= 1;
                end

                case (active_engine)
                    2'd0: begin // SMB2
                        smb_rx_data  <= sock_rx_data;
                        smb_rx_valid <= 1;
                        smb_rx_sof   <= sock_rx_sof;
                        smb_rx_eof   <= sock_rx_eof;
                        smb_rx_conn  <= sock_rx_id;
                    end
                    2'd1: begin // HTTP
                        http_rx_data  <= sock_rx_data;
                        http_rx_valid <= 1;
                        http_rx_sof   <= sock_rx_sof;
                        http_rx_eof   <= sock_rx_eof;
                        http_rx_conn  <= sock_rx_id;
                    end
                    2'd2: begin // DB
                        db_rx_data  <= sock_rx_data;
                        db_rx_valid <= 1;
                        db_rx_sof   <= sock_rx_sof;
                        db_rx_eof   <= sock_rx_eof;
                        db_rx_conn  <= sock_rx_id;
                    end
                    2'd3: begin // Pico
                        pico_rx_data  <= sock_rx_data;
                        pico_rx_valid <= 1;
                        pico_rx_sof   <= sock_rx_sof;
                        pico_rx_eof   <= sock_rx_eof;
                        pico_rx_conn  <= sock_rx_id;
                    end
                endcase
            end
        end
    end

    // ── TX mux: round-robin arbitrate engine responses ──
    reg [1:0] tx_arb;  // current arbitration winner
    reg       tx_locked;
    reg [1:0] tx_lock_eng;

    // Which engines want to send?
    wire [3:0] tx_req = {pico_tx_valid, db_tx_valid, http_tx_valid, smb_tx_valid};

    assign smb_tx_ready  = sock_tx_ready && tx_locked && (tx_lock_eng == 2'd0);
    assign http_tx_ready = sock_tx_ready && tx_locked && (tx_lock_eng == 2'd1);
    assign db_tx_ready   = sock_tx_ready && tx_locked && (tx_lock_eng == 2'd2);
    assign pico_tx_ready = sock_tx_ready && tx_locked && (tx_lock_eng == 2'd3);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sock_tx_valid <= 0;
            tx_locked <= 0;
            tx_arb <= 0;
        end else begin
            sock_tx_valid <= 0;
            sock_tx_sof <= 0;
            sock_tx_eof <= 0;

            if (!tx_locked) begin
                // Round-robin: find next engine with data
                if (tx_req != 0) begin
                    if      (tx_req[tx_arb])          tx_lock_eng <= tx_arb;
                    else if (tx_req[(tx_arb+1) & 3])  tx_lock_eng <= (tx_arb+1) & 3;
                    else if (tx_req[(tx_arb+2) & 3])  tx_lock_eng <= (tx_arb+2) & 3;
                    else                              tx_lock_eng <= (tx_arb+3) & 3;
                    tx_locked <= 1;
                end
            end else begin
                // Forward from locked engine
                case (tx_lock_eng)
                    2'd0: begin
                        sock_tx_data <= smb_tx_data; sock_tx_valid <= smb_tx_valid;
                        sock_tx_sof <= smb_tx_sof; sock_tx_eof <= smb_tx_eof;
                        sock_tx_id <= smb_tx_conn;
                        if (smb_tx_eof && smb_tx_valid) begin
                            tx_locked <= 0; tx_arb <= 2'd1;
                        end
                    end
                    2'd1: begin
                        sock_tx_data <= http_tx_data; sock_tx_valid <= http_tx_valid;
                        sock_tx_sof <= http_tx_sof; sock_tx_eof <= http_tx_eof;
                        sock_tx_id <= http_tx_conn;
                        if (http_tx_eof && http_tx_valid) begin
                            tx_locked <= 0; tx_arb <= 2'd2;
                        end
                    end
                    2'd2: begin
                        sock_tx_data <= db_tx_data; sock_tx_valid <= db_tx_valid;
                        sock_tx_sof <= db_tx_sof; sock_tx_eof <= db_tx_eof;
                        sock_tx_id <= db_tx_conn;
                        if (db_tx_eof && db_tx_valid) begin
                            tx_locked <= 0; tx_arb <= 2'd3;
                        end
                    end
                    2'd3: begin
                        sock_tx_data <= pico_tx_data; sock_tx_valid <= pico_tx_valid;
                        sock_tx_sof <= pico_tx_sof; sock_tx_eof <= pico_tx_eof;
                        sock_tx_id <= pico_tx_conn;
                        if (pico_tx_eof && pico_tx_valid) begin
                            tx_locked <= 0; tx_arb <= 2'd0;
                        end
                    end
                endcase
            end
        end
    end

endmodule
`default_nettype wire
