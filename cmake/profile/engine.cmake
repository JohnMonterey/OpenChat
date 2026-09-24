# UE Engine hygiene: SyncEngine drain backpressure, outbox pruning and tst_outboxretention.

# Settled control envelopes leave the outbox (SqlCipherSyncStore), checked on
# disk through a second connection, and through a real SyncEngine.
add_executable(tst_outboxretention tests/tst_outboxretention.cpp)
target_include_directories(tst_outboxretention PRIVATE src)
target_link_libraries(tst_outboxretention PRIVATE
    openchat_storage openchat_network openchat_protocol openchat_domain Qt6::Core Qt6::Test)
add_test(NAME tst_outboxretention COMMAND tst_outboxretention)
