# SPDX-License-Identifier: MIT
"""Port planning avoids dynamic allocation and rejects occupied blocks."""
import unittest
from unittest.mock import patch

import process_support


class PortTests(unittest.TestCase):
    def test_explicit_block_does_not_request_ephemeral_ports(self):
        created = []

        class Socket:
            def __init__(self, *_):
                self.bound = None
                self.closed = False
                created.append(self)

            def bind(self, address):
                self.bound = address
                if address[1] == 0:
                    raise AssertionError("ephemeral allocation can steal a planned sibling port")

            def close(self):
                self.closed = True

        with patch.object(process_support.socket, "socket", Socket), patch.object(
                process_support.secrets, "randbelow", return_value=19999):
            self.assertEqual(process_support.ports(6), 29999)
        self.assertEqual([s.bound for s in created], [("127.0.0.1", p) for p in range(29999, 30005)])
        self.assertTrue(all(s.closed for s in created))

    def test_busy_block_is_rejected_and_every_probe_is_closed(self):
        created = []

        class Socket:
            def __init__(self, *_):
                self.closed = False
                created.append(self)

            def bind(self, address):
                if address[1] == 10101:
                    raise OSError("occupied")

            def close(self):
                self.closed = True

        with patch.object(process_support.socket, "socket", Socket), patch.object(
                process_support.secrets, "randbelow", side_effect=[100, 200]):
            self.assertEqual(process_support.ports(3), 10200)
        self.assertEqual(len(created), 5)
        self.assertTrue(all(s.closed for s in created))

    def test_count_is_bounded(self):
        for count in (0, -1, 65):
            with self.subTest(count=count), self.assertRaises(ValueError):
                process_support.ports(count)


if __name__ == "__main__":
    unittest.main()
