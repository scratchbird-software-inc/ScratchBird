// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.hibernate.examples;

import jakarta.persistence.Column;
import jakarta.persistence.Entity;
import jakarta.persistence.EntityManager;
import jakarta.persistence.EntityManagerFactory;
import jakarta.persistence.GeneratedValue;
import jakarta.persistence.GenerationType;
import jakarta.persistence.Id;
import jakarta.persistence.Table;

/**
 * Deterministic lifecycle example for ScratchBird + Hibernate.
 *
 * Runtime execution requires a configured persistence unit.
 */
public final class ScratchBirdEntityLifecycleExample {

  @Entity
  @Table(name = "users", schema = "sys")
  public static class UserEntity {
    @Id
    @GeneratedValue(strategy = GenerationType.IDENTITY)
    @Column(name = "id")
    public Long id;

    @Column(name = "email", nullable = false)
    public String email;
  }

  private ScratchBirdEntityLifecycleExample() {}

  public static void run(EntityManagerFactory emf) {
    EntityManager em = emf.createEntityManager();
    try {
      em.getTransaction().begin();
      UserEntity created = new UserEntity();
      created.email = "user@example.com";
      em.persist(created);
      em.getTransaction().commit();

      em.getTransaction().begin();
      UserEntity loaded = em.find(UserEntity.class, created.id);
      loaded.email = "updated@example.com";
      em.getTransaction().commit();

      em.getTransaction().begin();
      UserEntity toDelete = em.find(UserEntity.class, created.id);
      if (toDelete != null) {
        em.remove(toDelete);
      }
      em.getTransaction().commit();
    } finally {
      em.close();
    }
  }
}
