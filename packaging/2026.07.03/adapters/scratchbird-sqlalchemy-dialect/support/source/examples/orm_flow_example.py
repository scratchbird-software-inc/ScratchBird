# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""ScratchBird SQLAlchemy ORM flow example.

This script shows a representative ORM lifecycle using the ScratchBird dialect.
Running it requires a live ScratchBird DSN.
"""

from sqlalchemy import Column, ForeignKey, Integer, String, create_engine, select
from sqlalchemy.orm import DeclarativeBase, Session, relationship


class Base(DeclarativeBase):
    pass


class User(Base):
    __tablename__ = "users"
    __table_args__ = {"schema": "sys"}

    id = Column(Integer, primary_key=True, autoincrement=True)
    email = Column(String(320), nullable=False)
    posts = relationship("Post", back_populates="user")


class Post(Base):
    __tablename__ = "posts"
    __table_args__ = {"schema": "sys"}

    id = Column(Integer, primary_key=True, autoincrement=True)
    user_id = Column(Integer, ForeignKey("sys.users.id"), nullable=False)
    title = Column(String(255), nullable=False)
    user = relationship("User", back_populates="posts")


def main() -> None:
    engine = create_engine(
        "scratchbird://sb_admin:change_me@127.0.0.1:3092/main?sslmode=require&binaryTransfer=true"
    )

    with Session(engine) as session:
        user = User(email="user@example.com")
        session.add(user)
        session.flush()

        post = Post(user_id=user.id, title="Hello ScratchBird")
        session.add(post)
        session.commit()

        rows = session.execute(select(User).where(User.email == "user@example.com")).scalars().all()
        print(f"rows={len(rows)}")


if __name__ == "__main__":
    main()
