<?php
  echo "1. PHP: " . dbsync_send('PING') . "\n";
  sleep(5);
  echo "2. PHP: " . dbsync_send('PING') . "\n";
?>
