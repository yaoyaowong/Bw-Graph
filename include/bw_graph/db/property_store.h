#ifndef BW_GRAPH_DB_PROPERTY_STORE_H
#define BW_GRAPH_DB_PROPERTY_STORE_H

#include "bw_graph/common/type.h"

#include <cstdint>
#include <optional>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>
#include <string>
#include <unordered_map>

/**
 * @brief LSM-Tree backed property storage for vertices and edges.
 *
 * Key encoding (binary, big-endian numerics for correct lexicographic order):
 *
 *   Vertex property:
 *     [tag:1B=0x01][vertex_id:4B BE][prop_key_len:2B BE][prop_key:N B]
 *
 *   Edge property:
 *     [tag:1B=0x02][src_id:4B BE][dst_id:4B BE][prop_key_len:2B BE][prop_key:N
 * B]
 *
 * Prefix scan ranges:
 *   - All properties of vertex V  : prefix = [0x01][V BE]        (5 bytes)
 *   - All properties of edge(s,d) : prefix = [0x02][s BE][d BE]  (9 bytes)
 *
 * Property values are stored as raw bytes (caller chooses encoding).
 */
class property_store_t {
public:
  static constexpr uint8_t VERTEX_PROP_TAG = 0x01;
  static constexpr uint8_t EDGE_PROP_TAG = 0x02;

  /**
   * @brief Open (or create) the property store at the given path.
   * @param db_path Path to the RocksDB directory.
   */
  explicit property_store_t(const std::string& db_path);

  ~property_store_t();

  // ── Vertex property operations ──────────────────────────────────────────

  /**
   * @brief Write (insert or overwrite) a single vertex property.
   * @param vertex_id  Target vertex.
   * @param prop_key   Property name (arbitrary bytes, max 65535 bytes).
   * @param prop_value Property value (arbitrary bytes).
   * @return true on success.
   */
  bool set_vertex_property(v_id_t vertex_id, const std::string& prop_key,
                           const std::string& prop_value);

  /**
   * @brief Read a single vertex property.
   * @return The value, or std::nullopt if the property does not exist.
   */
  std::optional<std::string> get_vertex_property(v_id_t vertex_id, const std::string& prop_key);

  /**
   * @brief Delete a single vertex property.
   * @return true on success (including the case where the key didn't exist).
   */
  bool delete_vertex_property(v_id_t vertex_id, const std::string& prop_key);

  /**
   * @brief Retrieve all properties of a vertex via prefix scan.
   * @return Map from property name to value.
   */
  std::unordered_map<std::string, std::string> get_all_vertex_properties(v_id_t vertex_id);

  /**
   * @brief Delete all properties of a vertex via prefix scan + batch delete.
   * @return true on success.
   */
  bool delete_all_vertex_properties(v_id_t vertex_id);

  /**
   * @brief Write multiple vertex properties in a single atomic batch.
   * @param vertex_id  Target vertex.
   * @param properties Map of property name → value to write.
   * @return true on success.
   */
  bool set_vertex_properties(v_id_t vertex_id,
                             const std::unordered_map<std::string, std::string>& properties);

  // ── Edge property operations ─────────────────────────────────────────────

  /**
   * @brief Write (insert or overwrite) a single edge property.
   * @param src_id     Source vertex ID of the edge.
   * @param dst_id     Destination vertex ID of the edge.
   * @param prop_key   Property name (arbitrary bytes, max 65535 bytes).
   * @param prop_value Property value (arbitrary bytes).
   * @return true on success.
   */
  bool set_edge_property(v_id_t src_id, v_id_t dst_id, const std::string& prop_key,
                         const std::string& prop_value);

  /**
   * @brief Read a single edge property.
   * @return The value, or std::nullopt if the property does not exist.
   */
  std::optional<std::string> get_edge_property(v_id_t src_id, v_id_t dst_id,
                                               const std::string& prop_key);

  /**
   * @brief Delete a single edge property.
   * @return true on success.
   */
  bool delete_edge_property(v_id_t src_id, v_id_t dst_id, const std::string& prop_key);

  /**
   * @brief Retrieve all properties of an edge via prefix scan.
   * @return Map from property name to value.
   */
  std::unordered_map<std::string, std::string> get_all_edge_properties(v_id_t src_id,
                                                                       v_id_t dst_id);

  /**
   * @brief Delete all properties of an edge via prefix scan + batch delete.
   * @return true on success.
   */
  bool delete_all_edge_properties(v_id_t src_id, v_id_t dst_id);

  /**
   * @brief Write multiple edge properties in a single atomic batch.
   * @return true on success.
   */
  bool set_edge_properties(v_id_t src_id, v_id_t dst_id,
                           const std::unordered_map<std::string, std::string>& properties);

  // ── Maintenance ──────────────────────────────────────────────────────────

  /** @brief Flush the write-ahead log for durability. */
  void flush_wal();

  /** @brief Trigger a manual compaction to reclaim space. */
  void compact();

private:
  rocksdb::DB* db_{nullptr};
  rocksdb::Options options_;
  std::string db_path_;

  void initialize_options();

  // ── Key builders ─────────────────────────────────────────────────────────

  /**
   * @brief Build the full RocksDB key for a vertex property.
   *   Layout: [0x01][v_id BE 4B][prop_key_len BE 2B][prop_key]
   */
  std::string make_vertex_prop_key(v_id_t vertex_id, const std::string& prop_key) const;

  /**
   * @brief Build the prefix used to scan all properties of a vertex.
   *   Layout: [0x01][v_id BE 4B]  (5 bytes)
   */
  std::string make_vertex_prefix(v_id_t vertex_id) const;

  /**
   * @brief Build the full RocksDB key for an edge property.
   *   Layout: [0x02][src BE 4B][dst BE 4B][prop_key_len BE 2B][prop_key]
   */
  std::string make_edge_prop_key(v_id_t src_id, v_id_t dst_id, const std::string& prop_key) const;

  /**
   * @brief Build the prefix used to scan all properties of an edge.
   *   Layout: [0x02][src BE 4B][dst BE 4B]  (9 bytes)
   */
  std::string make_edge_prefix(v_id_t src_id, v_id_t dst_id) const;

  /**
   * @brief Extract the property key name from a raw RocksDB key.
   *   Skips the fixed-size prefix (tag + IDs + prop_key_len).
   * @param raw_key Full key returned by an iterator.
   * @param prefix_len Length of the fixed prefix (5 for vertex, 9 for edge).
   * @return The property name, or empty string on malformed input.
   */
  std::string extract_prop_name(const rocksdb::Slice& raw_key, size_t prefix_len) const;

  // ── Byte-order helpers ───────────────────────────────────────────────────

  static void write_be32(char* buf, uint32_t v);
  static void write_be16(char* buf, uint16_t v);
};

#endif // BW_GRAPH_DB_PROPERTY_STORE_H
