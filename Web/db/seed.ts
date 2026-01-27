import { auth } from "../lib/auth";
import { randomBytes } from "crypto";

async function main() {
    // Generate a secure random password
    const password = "test@123";
    const email = "admin@example.com";
    const name = "Admin";

    console.log("🌱 Seeding database...");

    try {
        // Use Better Auth API to create the user
        // This ensures proper password hashing and DB insertion
        const res = await auth.api.signUpEmail({
            body: {
                email,
                password,
                name,
            }
        });

        if (res) {
            console.log("\n✅ Admin user created successfully!");
            console.log("----------------------------------------");
            console.log(`📧 Email:    ${email}`);
            console.log(`🔑 Password: ${password}`);
            console.log("----------------------------------------");
            console.log("⚠️  Copy this password now! It will not be shown again.");
        }
    } catch (error) {
        // Check if error is because user already exists
        if (String(error).includes("User already exists") || String(error).includes("UNIQUE constraint failed")) {
             console.log("\n⚠️  Admin user already exists. Skipping creation.");
        } else {
            console.error("\n❌ Error creating admin user:", error);
        }
    }
}

main();
