import 'dotenv/config';
import { db } from './index';
import { sql } from 'drizzle-orm';

async function reset() {
  console.log('⏳ Resetting database...');

  try {
    // Disable foreign key checks to allow truncation
    await db.run(sql`PRAGMA foreign_keys = OFF`);

    const tables = ['user', 'session', 'account', 'verification', 'device', 'device_log', 'device_metrics', 'audit_log'];

    for (const table of tables) {
      await db.run(sql.raw(`DELETE FROM ${table}`));
      // Reset autoincrement
      await db.run(sql.raw(`DELETE FROM sqlite_sequence WHERE name='${table}'`));
    }

    await db.run(sql`PRAGMA foreign_keys = ON`);

    console.log('✅ Database reset complete');
    process.exit(0);
  } catch (err) {
    console.error('❌ Reset failed:', err);
    process.exit(1);
  }
}

reset();
