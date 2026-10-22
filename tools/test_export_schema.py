#!/usr/bin/env python3
"""Tests for stable schema snapshot export."""
from __future__ import annotations

import unittest

from export_schema import normalizeDefiners


class DefinerNormalizationTests(unittest.TestCase):
    def test_removes_account_specific_trigger_and_view_definers(self) -> None:
        dump = (
            "/*!50003 CREATE*/ /*!50017 DEFINER=`root`@`%`*/ "
            "/*!50003 TRIGGER trigger_name */\n"
            "/*!50001 CREATE ALGORITHM=UNDEFINED */\n"
            "/*!50013 DEFINER=`deploy``user`@`db-host` SQL SECURITY DEFINER */\n"
            "VIEW view_name AS SELECT 1;\n"
        )

        result = normalizeDefiners(dump)

        self.assertNotIn("DEFINER=", result)
        self.assertIn("/*!50003 TRIGGER trigger_name */", result)
        self.assertIn("/*!50013 SQL SECURITY DEFINER */", result)
        self.assertIn("VIEW view_name AS SELECT 1;", result)

    def test_keeps_invoker_security_and_unrelated_dump_text(self) -> None:
        dump = (
            "/*!50013 DEFINER=`root`@`localhost` SQL SECURITY INVOKER */\n"
            "SELECT 'DEFINER=literal' AS note;\n"
        )

        result = normalizeDefiners(dump)

        self.assertIn("/*!50013 SQL SECURITY INVOKER */", result)
        self.assertIn("SELECT 'DEFINER=literal' AS note;", result)

    def test_normalizes_mariadb_note_verbosity_terminator(self) -> None:
        line = "/*M!100616 SET @OLD_NOTE_VERBOSITY=@@NOTE_VERBOSITY, NOTE_VERBOSITY=0 */"

        self.assertEqual(normalizeDefiners(line + "\n"), line + ";\n")
        self.assertEqual(normalizeDefiners(line + ";\n"), line + ";\n")
        self.assertEqual(normalizeDefiners(line + "  \r\n"), line + ";\r\n")


if __name__ == "__main__":
    unittest.main()
