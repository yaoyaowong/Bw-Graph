#ifndef BW_GRAPH_DB_EXTERNAL_H
#define BW_GRAPH_DB_EXTERNAL_H

#include "bw_graph/common/type.h"
#include "bw_graph/part/smo.h"

#include <cstdint>
#include <memory>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>
#include <string>
#include <sys/types.h>
#include <tbb/concurrent_hash_map.h>
#include <tbb/concurrent_queue.h>
#include <tbb/concurrent_vector.h>
#include <tbb/parallel_for.h>
#include <tbb/task_group.h>
#include <utility>
#include <yaml-cpp/yaml.h>

/**
 * @brief: Represents a change in the neighbor list of a vertex
 */
struct neighbor_delta_t {
  delta_operation_t operation;
  v_id_t neighbor_id;
  uint64_t timestamp;

  neighbor_delta_t(delta_operation_t op, v_id_t neighbor, uint64_t ts = 0)
      : operation(op), neighbor_id(neighbor), timestamp(ts) {}
};

/**
 * @brief: The RocksDB-based graph database class for vertex storage
 */
class giant_vertex_db_t {
private:
  /**
   * @brief: The RocksDB database instance
   */
  rocksdb::DB* db_{nullptr};

  /**
   * @brief: Database configuration options
   */
  rocksdb::Options options_;

  /**
   * @brief: The database path
   */
  std::string db_path_;

  /**
   * @brief: Total vertex count in the graph
   */
  uint64_t vertex_count_{0};

  /**
   * @brief: Cache for neighbor lists
   */
  folly::ConcurrentHashMap<v_id_t, std::shared_ptr<std::vector<v_id_t>>> neighbor_cache;

  /**
   * @brief: Initialize RocksDB with performance-optimized settings
   */
  void initialize_options();

  /**
   * @brief: Generate key string for vertex storage
   * @param vertex_id: The vertex identifier
   * @return: Formatted key string
   */
  std::string make_vertex_key(v_id_t vertex_id) const;

  /**
   * @brief: Serialize neighbor list to binary format
   * @param neighbors: Vector of neighbor vertex IDs
   * @return: Binary serialized string
   */
  std::string serialize_neighbors(const std::vector<v_id_t>& neighbors) const;

  /**
   * @brief: Deserialize binary data to neighbor list
   * @param data: Binary serialized string
   * @return: Vector of neighbor vertex IDs
   */
  std::vector<v_id_t> deserialize_neighbors(const std::string& data) const;

  /**
   * @brief: Generate delta key string for vertex operations
   * @param vertex_id: The vertex identifier
   * @param timestamp: Timestamp for ordering (microseconds since epoch)
   * @return: Formatted delta key string
   */
  std::string make_delta_key(v_id_t vertex_id, uint64_t timestamp) const;

  /**
   * @brief: Serialize delta operation to binary format
   * @param delta: The delta operation to serialize
   * @return: Binary serialized string
   */
  std::string serialize_delta(const neighbor_delta_t& delta) const;

  /**
   * @brief: Deserialize binary data to delta operation
   * @param data: Binary serialized string
   * @return: Delta operation
   */
  neighbor_delta_t deserialize_delta(const std::string& data) const;

  /**
   * @brief: Get current timestamp in microseconds
   * @return: Current timestamp
   */
  uint64_t get_current_timestamp() const;

  /**
   * @brief: Apply accumulated deltas to get current neighbor list
   * @param vertex_id: The vertex identifier
   * @return: Current neighbor list after applying all deltas
   */
  std::vector<v_id_t> read_neighbors_with_deltas(v_id_t vertex_id);

public:
  /**
   * @brief: The name of the stored graph
   */
  std::string graph_name_;

  /**
   * @brief: The constructor for the graph database
   * @param graph_name: The name of the graph
   * @param db_path: Path to the RocksDB database directory
   */
  giant_vertex_db_t(const std::string& graph_name, const std::string& db_path);

  /**
   * @brief: The destructor for the graph database
   */
  ~giant_vertex_db_t();

  /**
   * @brief: Insert a vertex with its neighbors
   * @param vertex_id: The ID of the vertex to insert
   * @param neighbors: Vector of neighbor vertex IDs
   * @return: True if insertion successful, false otherwise
   */
  bool insert_vertex(v_id_t vertex_id, const std::vector<v_id_t>& neighbors);

  /**
   * @brief: Read the neighbors of a vertex
   * @param vertex_id: The ID of the vertex
   * @return: A vector of neighbor vertex IDs
   */
  std::vector<v_id_t> read_neighbor_clone(v_id_t vertex_id);

  /**
   * @brief: Read the neighbors of a vertex with zero copy;
   * @param vertex_id: The ID of the vertex
   * @return: A span of neighbor vertex IDs
   */
  neighbor_span_t read_neighbor(v_id_t vertex_id);

  /**
   * @brief: Add a neighbor to an existing vertex
   * @param vertex_id: The ID of the vertex
   * @param neighbor_id: The ID of the neighbor to add
   * @return: True if addition successful, false otherwise
   */
  bool add_neighbor(v_id_t vertex_id, v_id_t neighbor_id);

  /**
   * @brief: Remove a neighbor from an existing vertex
   * @param vertex_id: The ID of the vertex
   * @param neighbor_id: The ID of the neighbor to remove
   * @return: True if removal successful, false otherwise
   */
  bool remove_neighbor(v_id_t vertex_id, v_id_t neighbor_id);

  /**
   * @brief: Batch insert multiple vertices with their neighbors
   * @param vertices: Vector of pairs containing vertex ID and its neighbors
   * @return: True if batch insertion successful, false otherwise
   */
  bool batch_insert_vertices(const std::vector<std::pair<v_id_t, std::vector<v_id_t>>>& vertices);

  /**
   * @brief: Check if a vertex exists in the database
   * @param vertex_id: The ID of the vertex to check
   * @return: True if vertex exists, false otherwise
   */
  bool vertex_exists(v_id_t vertex_id);

  /**
   * @brief: Get the degree (number of neighbors) of a vertex
   * @param vertex_id: The ID of the vertex
   * @return: The degree of the vertex, 0 if vertex doesn't exist
   */
  uint64_t get_vertex_degree(v_id_t vertex_id);

  /**
   * @brief: Get the count of vertices in the graph
   * @return: The number of vertices
   */
  uint64_t get_vertex_count() const;

  /**
   * @brief: Set the total vertex count (for metadata tracking)
   * @param count: The total vertex count
   */
  void set_vertex_count(uint64_t count);

  /**
   * @brief: Print RocksDB internal statistics for performance monitoring
   */
  void print_stats();

  /**
   * @brief: Flush write-ahead log to ensure durability
   */
  void flush_wal();

  /**
   * @brief: Compact database to optimize storage and performance
   */
  void compact_database();

  /**
   * @brief: Add a neighbor using delta operation (write-only, no read)
   * @param vertex_id: The ID of the vertex
   * @param neighbor_id: The ID of the neighbor to add
   * @return: True if delta insertion successful, false otherwise
   */
  bool add_neighbor_delta(v_id_t vertex_id, v_id_t neighbor_id);

  /**
   * @brief: Remove a neighbor using delta operation (write-only, no read)
   * @param vertex_id: The ID of the vertex
   * @param neighbor_id: The ID of the neighbor to remove
   * @return: True if delta insertion successful, false otherwise
   */
  bool remove_neighbor_delta(v_id_t vertex_id, v_id_t neighbor_id);

  /**
   * @brief: Batch apply delta operations for better performance
   * @param vertex_deltas: Vector of pairs containing vertex ID and delta
   * operations
   * @return: True if batch insertion successful, false otherwise
   */
  bool batch_apply_deltas(
      const std::vector<std::pair<v_id_t, std::vector<neighbor_delta_t>>>& vertex_deltas);

  /**
   * @brief: Consolidate deltas for a vertex into the main neighbor list
   * @param vertex_id: The vertex to consolidate
   * @return: True if consolidation successful, false otherwise
   */
  bool consolidate_vertex_deltas(v_id_t vertex_id);

  /**
   * @brief: Read neighbors with delta operations applied (slower but accurate)
   * @param vertex_id: The ID of the vertex
   * @return: Current neighbor list with all deltas applied
   */
  std::vector<v_id_t> read_neighbors_with_deltas_applied(v_id_t vertex_id);

private:
  /**
   * @brief Compress neighbors using Delta+VByte encoding
   * @param neighbors Sorted neighbor list
   * @return Compressed byte array
   */
  std::vector<uint8_t> compress_neighbors(const std::vector<v_id_t>& neighbors) const;

  /**
   * @brief Decompress neighbors from Delta+VByte encoding
   * @param data Compressed data pointer
   * @param degree Number of neighbors (original count)
   * @return Decompressed neighbor list
   */
  std::vector<v_id_t> decompress_neighbors(const uint8_t* data, uint64_t degree) const;

  /**
   * @brief Encode a value using VByte encoding
   * @param value Value to encode
   * @param output Output byte vector
   */
  void encode_vbyte(uint64_t value, std::vector<uint8_t>& output) const;

  /**
   * @brief Decode a VByte encoded value
   * @param ptr Pointer to encoded data (will be advanced)
   * @return Decoded value
   */
  uint64_t decode_vbyte(const uint8_t*& ptr) const;
};

#endif // BW_GRAPH_DB_EXTERNAL_H