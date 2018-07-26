<?php
  echo "Test keepalived driver connection";
  echo "1. First ping call: " . dbsync_send('PING') . "\n";
  echo "2. Immediate second ping call: " . dbsync_send('PING') . "\n";
  echo "3. 5 seconds delay\n";
  sleep(5);
  echo "4. Ping call after long delay (should fail): " . dbsync_send('PING') . "\n";
  dbsync_reset();
  echo "5. Ping call after connection reset: " . dbsync_send('PING') . "\n";
?>
