import os
import sys
from pathlib import Path

import pytest


@pytest.fixture
def db_path(tmp_path_factory) -> Path:
    return tmp_path_factory.mktemp("wavedb") / "db"