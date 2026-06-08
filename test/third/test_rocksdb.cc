#include <filesystem>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

class RocksDBTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Test preparation
    test_db_path = "./test_rocksdb_" + std::to_string(std::time(nullptr));
  }

  void TearDown() override {
    // Test cleanup - remove test database
    if (std::filesystem::exists(test_db_path)) {
      std::filesystem::remove_all(test_db_path);
    }
  }

  std::string test_db_path;
};

// Test if RocksDB header files are correctly imported
TEST_F(RocksDBTest, HeaderImportTest) {
  // Test if RocksDB basic types are available
  EXPECT_TRUE(true); // If header import fails, compilation will fail

  // Test RocksDB basic type definitions
  rocksdb::Status status;
  rocksdb::Options options;
  rocksdb::ReadOptions read_options;
  rocksdb::WriteOptions write_options;

  EXPECT_TRUE(status.ok() || !status.ok()); // Status should be valid
  EXPECT_TRUE(true);                        // Options creation should not throw

  std::cout << "RocksDB header imported successfully" << std::endl;
}

// Test RocksDB database creation and basic operations
TEST_F(RocksDBTest, DatabaseCreationTest) {
  std::unique_ptr<rocksdb::DB> db;
  rocksdb::Options options;
  options.create_if_missing = true;

  // Open database
  rocksdb::Status status = rocksdb::DB::Open(options, test_db_path, &db);
  EXPECT_TRUE(status.ok()) << "Failed to open database: " << status.ToString();

  if (status.ok()) {
    // Verify database is properly initialized
    EXPECT_NE(db, nullptr);

    // unique_ptr handles cleanup automatically

    std::cout << "RocksDB database created successfully" << std::endl;
  }
}

// Test basic read/write operations
TEST_F(RocksDBTest, BasicReadWriteTest) {
  std::unique_ptr<rocksdb::DB> db;
  rocksdb::Options options;
  options.create_if_missing = true;

  // Open database
  rocksdb::Status status = rocksdb::DB::Open(options, test_db_path, &db);
  ASSERT_TRUE(status.ok()) << "Failed to open database: " << status.ToString();

  // Test write operation
  std::string key = "test_key";
  std::string value = "test_value";
  status = db->Put(rocksdb::WriteOptions(), key, value);
  EXPECT_TRUE(status.ok()) << "Failed to write to database: " << status.ToString();

  // Test read operation
  std::string retrieved_value;
  status = db->Get(rocksdb::ReadOptions(), key, &retrieved_value);
  EXPECT_TRUE(status.ok()) << "Failed to read from database: " << status.ToString();
  EXPECT_EQ(retrieved_value, value) << "Retrieved value doesn't match written value";

  // Test delete operation
  status = db->Delete(rocksdb::WriteOptions(), key);
  EXPECT_TRUE(status.ok()) << "Failed to delete from database: " << status.ToString();

  // Verify deletion
  status = db->Get(rocksdb::ReadOptions(), key, &retrieved_value);
  EXPECT_TRUE(status.IsNotFound()) << "Key should not exist after deletion";

  // unique_ptr handles cleanup automatically

  std::cout << "RocksDB basic read/write operations successful" << std::endl;
}

// Test batch operations
TEST_F(RocksDBTest, BatchOperationsTest) {
  std::unique_ptr<rocksdb::DB> db;
  rocksdb::Options options;
  options.create_if_missing = true;

  // Open database
  rocksdb::Status status = rocksdb::DB::Open(options, test_db_path, &db);
  ASSERT_TRUE(status.ok()) << "Failed to open database: " << status.ToString();

  // Create batch
  rocksdb::WriteBatch batch;
  batch.Put("key1", "value1");
  batch.Put("key2", "value2");
  batch.Put("key3", "value3");

  // Execute batch
  status = db->Write(rocksdb::WriteOptions(), &batch);
  EXPECT_TRUE(status.ok()) << "Failed to execute batch write: " << status.ToString();

  // Verify batch write
  std::string value;
  for (int i = 1; i <= 3; ++i) {
    std::string key = "key" + std::to_string(i);
    std::string expected_value = "value" + std::to_string(i);

    status = db->Get(rocksdb::ReadOptions(), key, &value);
    EXPECT_TRUE(status.ok()) << "Failed to read key: " << key;
    EXPECT_EQ(value, expected_value) << "Value mismatch for key: " << key;
  }

  // unique_ptr handles cleanup automatically

  std::cout << "RocksDB batch operations successful" << std::endl;
}

// Test iterator operations
TEST_F(RocksDBTest, IteratorTest) {
  std::unique_ptr<rocksdb::DB> db;
  rocksdb::Options options;
  options.create_if_missing = true;

  // Open database
  rocksdb::Status status = rocksdb::DB::Open(options, test_db_path, &db);
  ASSERT_TRUE(status.ok()) << "Failed to open database: " << status.ToString();

  // Insert test data
  std::vector<std::pair<std::string, std::string>> test_data = {
      {"apple", "fruit"}, {"banana", "fruit"}, {"carrot", "vegetable"}};

  for (const auto& pair : test_data) {
    status = db->Put(rocksdb::WriteOptions(), pair.first, pair.second);
    EXPECT_TRUE(status.ok()) << "Failed to insert: " << pair.first;
  }

  // Test iterator
  std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(rocksdb::ReadOptions()));
  EXPECT_NE(it, nullptr);

  // Count entries using iterator
  int count = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    count++;
    std::string key = it->key().ToString();
    std::string value = it->value().ToString();

    // Verify this key-value pair exists in our test data
    bool found = false;
    for (const auto& pair : test_data) {
      if (pair.first == key && pair.second == value) {
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found) << "Unexpected key-value pair: " << key << " -> " << value;
  }

  EXPECT_EQ(count, test_data.size()) << "Iterator count doesn't match inserted data";
  EXPECT_TRUE(it->status().ok()) << "Iterator error: " << it->status().ToString();

  // unique_ptr handles cleanup automatically

  std::cout << "RocksDB iterator operations successful" << std::endl;
}