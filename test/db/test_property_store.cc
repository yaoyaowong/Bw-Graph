#include "bw_graph/db/property_store.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <unordered_map>

class PropertyStoreTest : public ::testing::Test {
protected:
  std::string db_path;
  property_store_t* store{nullptr};

  void SetUp() override {
    db_path = "./test_propstore_" + std::to_string(std::time(nullptr));
    store = new property_store_t(db_path);
  }

  void TearDown() override {
    delete store;
    store = nullptr;
    if (std::filesystem::exists(db_path)) {
      std::filesystem::remove_all(db_path);
    }
  }
};

TEST_F(PropertyStoreTest, VertexSetAndGet) {
  EXPECT_TRUE(store->set_vertex_property(1, "name", "Alice"));
  auto val = store->get_vertex_property(1, "name");
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, "Alice");
}

// Test PropertyStoreTest.vertex get missing.
TEST_F(PropertyStoreTest, VertexGetMissing) {
  auto val = store->get_vertex_property(999, "age");
  EXPECT_FALSE(val.has_value());
}

// Test PropertyStoreTest.vertex overwrite.
TEST_F(PropertyStoreTest, VertexOverwrite) {
  EXPECT_TRUE(store->set_vertex_property(1, "age", "30"));
  EXPECT_TRUE(store->set_vertex_property(1, "age", "31"));
  auto val = store->get_vertex_property(1, "age");
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, "31");
}

TEST_F(PropertyStoreTest, VertexDeleteSingleProperty) {
  store->set_vertex_property(2, "label", "Person");
  EXPECT_TRUE(store->delete_vertex_property(2, "label"));
  EXPECT_FALSE(store->get_vertex_property(2, "label").has_value());
}

// Test PropertyStoreTest.vertex delete non existent property.
TEST_F(PropertyStoreTest, VertexDeleteNonExistentProperty) {
  // Deleting a missing key should succeed without error
  EXPECT_TRUE(store->delete_vertex_property(42, "missing"));
}

TEST_F(PropertyStoreTest, VertexBatchSetAndGetAll) {
  std::unordered_map<std::string, std::string> props = {
      {"name", "Bob"},
      {"age", "25"},
      {"city", "Berlin"},
  };
  EXPECT_TRUE(store->set_vertex_properties(10, props));

  auto result = store->get_all_vertex_properties(10);
  ASSERT_EQ(result.size(), 3u);
  EXPECT_EQ(result.at("name"), "Bob");
  EXPECT_EQ(result.at("age"), "25");
  EXPECT_EQ(result.at("city"), "Berlin");
}

// Test PropertyStoreTest.vertex get all empty.
TEST_F(PropertyStoreTest, VertexGetAllEmpty) {
  auto result = store->get_all_vertex_properties(777);
  EXPECT_TRUE(result.empty());
}

TEST_F(PropertyStoreTest, VertexDeleteAllProperties) {
  store->set_vertex_property(5, "x", "1");
  store->set_vertex_property(5, "y", "2");
  store->set_vertex_property(5, "z", "3");

  EXPECT_TRUE(store->delete_all_vertex_properties(5));

  auto result = store->get_all_vertex_properties(5);
  EXPECT_TRUE(result.empty());
}

// Test PropertyStoreTest.vertex delete all does not affect other vertices.
TEST_F(PropertyStoreTest, VertexDeleteAllDoesNotAffectOtherVertices) {
  store->set_vertex_property(6, "k", "v");
  store->set_vertex_property(7, "k", "v");

  store->delete_all_vertex_properties(6);

  EXPECT_FALSE(store->get_vertex_property(6, "k").has_value());
  EXPECT_TRUE(store->get_vertex_property(7, "k").has_value());
}

TEST_F(PropertyStoreTest, VertexIsolation) {
  store->set_vertex_property(100, "name", "Alice");
  store->set_vertex_property(101, "name", "Bob");

  EXPECT_EQ(*store->get_vertex_property(100, "name"), "Alice");
  EXPECT_EQ(*store->get_vertex_property(101, "name"), "Bob");

  auto a_props = store->get_all_vertex_properties(100);
  EXPECT_EQ(a_props.size(), 1u);
  EXPECT_EQ(a_props.at("name"), "Alice");
}

TEST_F(PropertyStoreTest, EdgeSetAndGet) {
  EXPECT_TRUE(store->set_edge_property(1, 2, "weight", "3.14"));
  auto val = store->get_edge_property(1, 2, "weight");
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, "3.14");
}

// Test PropertyStoreTest.edge get missing.
TEST_F(PropertyStoreTest, EdgeGetMissing) {
  auto val = store->get_edge_property(0, 0, "label");
  EXPECT_FALSE(val.has_value());
}

// Test PropertyStoreTest.edge overwrite.
TEST_F(PropertyStoreTest, EdgeOverwrite) {
  store->set_edge_property(3, 4, "type", "friend");
  store->set_edge_property(3, 4, "type", "colleague");
  auto val = store->get_edge_property(3, 4, "type");
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, "colleague");
}

TEST_F(PropertyStoreTest, EdgeDeleteSingleProperty) {
  store->set_edge_property(5, 6, "since", "2020");
  EXPECT_TRUE(store->delete_edge_property(5, 6, "since"));
  EXPECT_FALSE(store->get_edge_property(5, 6, "since").has_value());
}

TEST_F(PropertyStoreTest, EdgeBatchSetAndGetAll) {
  std::unordered_map<std::string, std::string> props = {
      {"weight", "1.5"},
      {"label", "KNOWS"},
      {"since", "2021"},
  };
  EXPECT_TRUE(store->set_edge_properties(10, 20, props));

  auto result = store->get_all_edge_properties(10, 20);
  ASSERT_EQ(result.size(), 3u);
  EXPECT_EQ(result.at("weight"), "1.5");
  EXPECT_EQ(result.at("label"), "KNOWS");
  EXPECT_EQ(result.at("since"), "2021");
}

// Test PropertyStoreTest.edge get all empty.
TEST_F(PropertyStoreTest, EdgeGetAllEmpty) {
  auto result = store->get_all_edge_properties(888, 999);
  EXPECT_TRUE(result.empty());
}

TEST_F(PropertyStoreTest, EdgeDeleteAllProperties) {
  store->set_edge_property(7, 8, "a", "1");
  store->set_edge_property(7, 8, "b", "2");
  EXPECT_TRUE(store->delete_all_edge_properties(7, 8));

  auto result = store->get_all_edge_properties(7, 8);
  EXPECT_TRUE(result.empty());
}

// Test PropertyStoreTest.edge delete all does not affect other edges.
TEST_F(PropertyStoreTest, EdgeDeleteAllDoesNotAffectOtherEdges) {
  store->set_edge_property(9, 10, "w", "5");
  store->set_edge_property(9, 11, "w", "6");

  store->delete_all_edge_properties(9, 10);

  EXPECT_FALSE(store->get_edge_property(9, 10, "w").has_value());
  EXPECT_TRUE(store->get_edge_property(9, 11, "w").has_value());
}

TEST_F(PropertyStoreTest, VertexAndEdgeKeysDoNotCollide) {
  // vertex 1 prop "name" and edge (1,2) prop "name" are different keys
  store->set_vertex_property(1, "name", "vertex_value");
  store->set_edge_property(1, 2, "name", "edge_value");

  EXPECT_EQ(*store->get_vertex_property(1, "name"), "vertex_value");
  EXPECT_EQ(*store->get_edge_property(1, 2, "name"), "edge_value");
}

TEST_F(PropertyStoreTest, BinaryPropertyValue) {
  // Values are raw bytes; test with embedded null bytes
  std::string bin_value("\x00\x01\x02\xFF", 4);
  EXPECT_TRUE(store->set_vertex_property(50, "blob", bin_value));
  auto val = store->get_vertex_property(50, "blob");
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, bin_value);
  EXPECT_EQ(val->size(), 4u);
}

TEST_F(PropertyStoreTest, ManyPropertiesOnOneVertex) {
  const int N = 100;
  for (int i = 0; i < N; ++i) {
    store->set_vertex_property(200, "prop_" + std::to_string(i), "val_" + std::to_string(i));
  }

  auto all = store->get_all_vertex_properties(200);
  EXPECT_EQ(static_cast<int>(all.size()), N);

  for (int i = 0; i < N; ++i) {
    EXPECT_EQ(all.at("prop_" + std::to_string(i)), "val_" + std::to_string(i));
  }
}

TEST_F(PropertyStoreTest, PersistenceAfterReopen) {
  store->set_vertex_property(300, "email", "test@example.com");
  store->set_edge_property(300, 301, "rel", "follows");
  // Flush before closing
  store->flush_wal();

  // Close and reopen
  delete store;
  store = new property_store_t(db_path);

  auto v_val = store->get_vertex_property(300, "email");
  ASSERT_TRUE(v_val.has_value());
  EXPECT_EQ(*v_val, "test@example.com");

  auto e_val = store->get_edge_property(300, 301, "rel");
  ASSERT_TRUE(e_val.has_value());
  EXPECT_EQ(*e_val, "follows");
}
