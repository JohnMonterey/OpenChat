# U5 Sync: ProfilePageSync and tst_profilepagesync.

# Publishing, the paced delivery pump, receiving and background requests for
# profile pages, over the ProfileUpdate envelopes of each contact's 1:1
# conversation. Qt Core only, like the rest of openchat_app.
target_sources(openchat_app PRIVATE
    src/app/ProfilePageSync.cpp
)
