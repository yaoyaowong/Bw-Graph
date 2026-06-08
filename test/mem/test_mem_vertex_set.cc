#include "bw_graph/mem/vertex_set.h"

#include <gtest/gtest.h>
#include <tuple>

// vertex_subset_t constructor cases.
TEST(VertexSubsetConstructorTest, DefaultConstructorCreatesEmptySubset) {
  vertex_subset_t subset(10);

  EXPECT_EQ(subset.num_vertices(), 10);
  EXPECT_EQ(subset.size(), 0);
  EXPECT_TRUE(subset.is_empty());
  EXPECT_FALSE(subset.dense());
}

// Test VertexSubsetConstructorTest.single vertex constructor creates sparse subset.
TEST(VertexSubsetConstructorTest, SingleVertexConstructorCreatesSparseSubset) {
  vertex_subset_t subset(8, 3);

  EXPECT_EQ(subset.num_vertices(), 8);
  EXPECT_EQ(subset.size(), 1);
  EXPECT_FALSE(subset.is_empty());
  EXPECT_FALSE(subset.dense());
  EXPECT_EQ(subset.vtx(0), 3);

  subset.del();
}

// Test VertexSubsetConstructorTest.dense constructor counts active vertices.
TEST(VertexSubsetConstructorTest, DenseConstructorCountsActiveVertices) {
  bool* dense = newA(bool, 6);
  dense[0] = false;
  dense[1] = true;
  dense[2] = false;
  dense[3] = true;
  dense[4] = true;
  dense[5] = false;

  vertex_subset_t subset(6, dense);

  EXPECT_EQ(subset.num_vertices(), 6);
  EXPECT_EQ(subset.size(), 3);
  EXPECT_TRUE(subset.dense());
  EXPECT_TRUE(subset.is_in(1));
  EXPECT_TRUE(subset.is_in(3));
  EXPECT_TRUE(subset.is_in(4));
  EXPECT_FALSE(subset.is_in(0));
  EXPECT_FALSE(subset.is_in(2));

  subset.del();
}

// vertex_subset_t Representation Conversion Tests
TEST(VertexSubsetConversionTest, ToDenseMarksSparseVerticesCorrectly) {
  v_id_t* vertices = newA(v_id_t, 3);
  vertices[0] = 1;
  vertices[1] = 4;
  vertices[2] = 6;

  vertex_subset_t subset(8, 3, vertices);
  subset.to_dense();

  EXPECT_TRUE(subset.dense());
  EXPECT_TRUE(subset.is_in(1));
  EXPECT_TRUE(subset.is_in(4));
  EXPECT_TRUE(subset.is_in(6));
  EXPECT_FALSE(subset.is_in(0));
  EXPECT_FALSE(subset.is_in(7));

  subset.del();
}

// Test VertexSubsetConversionTest.to sparse extracts dense vertices in order.
TEST(VertexSubsetConversionTest, ToSparseExtractsDenseVerticesInOrder) {
  bool* dense = newA(bool, 7);
  dense[0] = false;
  dense[1] = true;
  dense[2] = false;
  dense[3] = true;
  dense[4] = false;
  dense[5] = true;
  dense[6] = false;

  vertex_subset_t subset(7, dense);
  subset.to_sparse();

  EXPECT_FALSE(subset.dense());
  EXPECT_EQ(subset.size(), 3);
  EXPECT_EQ(subset.vtx(0), 1);
  EXPECT_EQ(subset.vtx(1), 3);
  EXPECT_EQ(subset.vtx(2), 5);

  subset.del();
}

// vertex_subset_t Access Tests
TEST(VertexSubsetAccessTest, GetFnReprReturnsCorrectSparseEntries) {
  v_id_t* vertices = newA(v_id_t, 2);
  vertices[0] = 2;
  vertices[1] = 5;

  vertex_subset_t subset(8, 2, vertices);
  auto fn = subset.get_fn_repr();

  auto first = fn(0);
  auto second = fn(1);

  EXPECT_TRUE(first.exists);
  EXPECT_TRUE(second.exists);
  EXPECT_EQ(std::get<0>(first.t), 2);
  EXPECT_EQ(std::get<0>(second.t), 5);

  subset.del();
}

// Test VertexSubsetAccessTest.get fn repr returns correct dense membership.
TEST(VertexSubsetAccessTest, GetFnReprReturnsCorrectDenseMembership) {
  bool* dense = newA(bool, 5);
  dense[0] = false;
  dense[1] = true;
  dense[2] = false;
  dense[3] = true;
  dense[4] = false;

  vertex_subset_t subset(5, dense);
  auto fn = subset.get_fn_repr();

  auto active = fn(3);
  auto inactive = fn(4);

  EXPECT_TRUE(active.exists);
  EXPECT_EQ(std::get<0>(active.t), 3);
  EXPECT_FALSE(inactive.exists);

  subset.del();
}

// vertex_subset_data_t<data> Tests
TEST(VertexSubsetDataTest, SparseSubsetStoresVertexDataCorrectly) {
  using data_type = int;
  using sparse_entry_t = std::tuple<v_id_t, data_type>;

  sparse_entry_t* entries = newA(sparse_entry_t, 3);
  entries[0] = std::make_tuple(1, 10);
  entries[1] = std::make_tuple(3, 20);
  entries[2] = std::make_tuple(4, 30);

  vertex_subset_data_t<data_type> subset(6, 3, entries);

  EXPECT_EQ(subset.num_vertices(), 6);
  EXPECT_EQ(subset.size(), 3);
  EXPECT_FALSE(subset.dense());
  EXPECT_EQ(subset.vtx(0), 1);
  EXPECT_EQ(subset.vtx_data(0), 10);
  EXPECT_EQ(subset.vtx(1), 3);
  EXPECT_EQ(subset.vtx_data(1), 20);
  EXPECT_EQ(subset.vtx(2), 4);
  EXPECT_EQ(subset.vtx_data(2), 30);

  subset.del();
}

// Test VertexSubsetDataTest.dense subset converts to sparse and preserves data.
TEST(VertexSubsetDataTest, DenseSubsetConvertsToSparseAndPreservesData) {
  using data_type = int;
  using dense_entry_t = std::tuple<bool, data_type>;

  dense_entry_t* dense = newA(dense_entry_t, 5);
  dense[0] = std::make_tuple(false, 0);
  dense[1] = std::make_tuple(true, 11);
  dense[2] = std::make_tuple(false, 0);
  dense[3] = std::make_tuple(true, 33);
  dense[4] = std::make_tuple(true, 44);

  vertex_subset_data_t<data_type> subset(5, dense);
  subset.to_sparse();

  EXPECT_FALSE(subset.dense());
  EXPECT_EQ(subset.size(), 3);
  EXPECT_EQ(subset.vtx(0), 1);
  EXPECT_EQ(subset.vtx_data(0), 11);
  EXPECT_EQ(subset.vtx(1), 3);
  EXPECT_EQ(subset.vtx_data(1), 33);
  EXPECT_EQ(subset.vtx(2), 4);
  EXPECT_EQ(subset.vtx_data(2), 44);

  subset.del();
}

// Test VertexSubsetDataTest.to dense preserves sparse data lookup.
TEST(VertexSubsetDataTest, ToDensePreservesSparseDataLookup) {
  using data_type = int;
  using sparse_entry_t = std::tuple<v_id_t, data_type>;

  sparse_entry_t* entries = newA(sparse_entry_t, 2);
  entries[0] = std::make_tuple(0, 7);
  entries[1] = std::make_tuple(4, 9);

  vertex_subset_data_t<data_type> subset(6, 2, entries);
  subset.to_dense();

  EXPECT_TRUE(subset.dense());
  EXPECT_TRUE(subset.is_in(0));
  EXPECT_TRUE(subset.is_in(4));
  EXPECT_FALSE(subset.is_in(2));
  EXPECT_EQ(subset.ith_data(0), 7);
  EXPECT_EQ(subset.ith_data(4), 9);

  subset.del();
}
