#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

// 	Necessary libraries to use the Paillier criptosystem
#include "gmp.h"
#include "paillier.h"

// 	OpenSSL is used to generate the hash and RSA encryption to be used in the squeme
#include "openssl/sha.h"
#include "openssl/rsa.h"
#include "openssl/pem.h"
#include "openssl/err.h"

int number_of_meters;
int *individual_measurements;

/* 
	In the Erkin squeme, only one pair of keys is shared among
	all smart meters
*/
paillier_pubkey_t *public_key;
paillier_prvkey_t *secret_key;

// A RSA keypair to encrypt the randomly generated numbers
RSA *key_pair;

//	Matrices containing generated and shared random numbers
char ***generated_numbers;
char ***received_numbers;

// 	Array of Ris from all meters
mpz_t *set_of_ri;

// 	time_stamp is needed to compute a hash, that must be unique
time_t time_stamp;
mpz_t the_hash;

// 	The measurements after encryption
paillier_ciphertext_t **enc_measurements;

/* 
	Store the total measurement after homomorphic operations
*/
mpz_t total_final;

/*
	Variables needed to compute the time to perform the operations
*/
double SM_time = 0;
double AG_time = 0;

struct meter_info{
	int id;
};

void init_variables(){

	srand(time(NULL));
	
	generated_numbers = calloc(number_of_meters, sizeof(char**));
	received_numbers = calloc(number_of_meters, sizeof(char**));
	
	key_pair = RSA_generate_key(2048, 3, NULL, NULL);

	int j;
	for(j = 0; j < number_of_meters; j++){
		generated_numbers[j] = calloc(number_of_meters, sizeof(char *));
		received_numbers[j] = calloc(number_of_meters, sizeof(char *));

		int k;
		for(k = 0; k < number_of_meters; k++){
			generated_numbers[j][k] = calloc(10, sizeof(char));
			received_numbers[j][k] = calloc(10, sizeof(char));
		}
	}

	enc_measurements = calloc(number_of_meters, sizeof(paillier_ciphertext_t *));
	set_of_ri = calloc(number_of_meters, sizeof(mpz_t));

	time_stamp = time(NULL);

	individual_measurements = calloc(number_of_meters, sizeof(int));
	
	// Generating random individual measurements
	int i;
	for(i = 0; i < number_of_meters; i++){

		individual_measurements[i] = rand() % 8000;
		
		mpz_init(set_of_ri[i]);
	}
	
	paillier_keygen(2048, &public_key, &secret_key, paillier_get_rand_devurandom);	

	mpz_init(total_final);

}

/*
	In step 2, we generate random numbers and share them using safe methods, like RSA encryption
*/ 
char* RSA_encrypt(int message){

	char str_message[10];
	sprintf(str_message, "%d", message);

//	str_message[strlen(str_message)-1] = '\0';

	char *encrypt = malloc(RSA_size(key_pair));
	int encrypt_len;
        char *err = malloc(130);

        if((encrypt_len = RSA_public_encrypt(
                strlen(str_message)+1,
                (unsigned char *)str_message,
                (unsigned char *)encrypt,
                key_pair,
                RSA_PKCS1_OAEP_PADDING)) == -1){

                ERR_load_CRYPTO_strings();
                ERR_error_string(ERR_get_error(), err);
                fprintf(stderr, "Error encrypting message: %s\n", err);

		return NULL;

        }

	return encrypt;
	
}

/*
	Used to decrypt the random numbers
*/
int RSA_decrypt(char * enc_message){

	char *err = malloc(130);
	char *decrypt = malloc(RSA_size(key_pair));

	if((RSA_private_decrypt(
                RSA_size(key_pair),
                (unsigned char *)enc_message,
                (unsigned char *)decrypt,
                key_pair,
				RSA_PKCS1_OAEP_PADDING) == -1)){

					ERR_load_crypto_strings();
                	ERR_error_string(ERR_get_error(), err);
                	fprintf(stderr, "Error decrypting message: %s\n", err);

                	return -1;

        }else{
                return atoi(decrypt);
        }
			
}

/*
	As part of the step 2 in the Erkin squeme,
	each meter must generate N-1 random numbers
	and share these
*/
void meter_generate_random_numbers(struct meter_info *info){

	int meter_index = info->id;

	int random_number;	

	int i;
	for(i = 0; i < number_of_meters; i++){
		if(i != meter_index){
			random_number = (rand() % 10000) - 5000;
//			random_number = rand() % RAND_MAX - (RAND_MAX / 2);

			char *enc_random_number = RSA_encrypt(random_number);
			generated_numbers[meter_index][i] = enc_random_number;
			received_numbers[i][meter_index] = enc_random_number;

		}else{
			char *enc_random_number = RSA_encrypt(0);
			generated_numbers[meter_index][i] = enc_random_number;
			received_numbers[i][meter_index] = enc_random_number;
		}


	}

}
/*
	Util function to calculate the GCD of two numbers
*/
long int gcd(mpz_t op1, mpz_t op2){
	// Initializing GMP variables
	mpz_t result;
	mpz_init(result);

	// Calculating GCD
	mpz_gcd(result, op1, op2);
	
	return mpz_get_ui(result);

}

/*
	Generate the ri number needed to encryption
*/
void meter_calculate_ri(struct meter_info *info){

	int meter_index = info->id;

	mpz_t ri;
	mpz_init(ri);

	mpz_set(ri, public_key->n);

	// Calculating the sum of the generated and received numbers;
	int generated_sum = 0;
	int received_sum = 0;

	int i;
	for(i = 0; i < number_of_meters; i++){
		generated_sum += RSA_decrypt(generated_numbers[meter_index][i]);
		received_sum += RSA_decrypt(received_numbers[meter_index][i]);
	}

	int random_sum = (generated_sum - received_sum);

	if(random_sum >= 0){
		mpz_add_ui(ri, ri, random_sum);
	}else{
		mpz_sub_ui(ri, ri, -1*random_sum);
	}

	mpz_set(set_of_ri[meter_index], ri);
}

/*
	Computes the hash needed to encryption
*/
void meter_compute_hash(){
	// Vars
	char data[20];
	sprintf(data, "%d", (int)time_stamp);
	unsigned char hash[SHA_DIGEST_LENGTH];
	unsigned char str_hash[SHA_DIGEST_LENGTH * 2 + 1];
	mpz_init(the_hash);

	// Computing the hash
	SHA1((unsigned char *)data, sizeof(data), (unsigned char *)hash);

	// Getting hash in str format
	char temp_str[SHA_DIGEST_LENGTH];

        strcpy(str_hash, "");
        strcpy(temp_str, "");

        int i;
        for(i = 0; i < SHA_DIGEST_LENGTH; i++){
                sprintf(temp_str, "%02x", hash[i]);
                strcat(str_hash, temp_str);
        }

	// Converting has from str format to mpz_t
	mpz_set_str(the_hash, str_hash, 16);
	
	// The generated hash must have gcd(hash , n) = 1
        while(gcd(the_hash, public_key->n) != 1){
                mpz_add_ui(the_hash, the_hash, 1);
        } 

}

/*
	Meter encrypts the measurement
*/
void meter_encript(struct meter_info *info, mpz_t ri){

	int meter_index = info->id;
	int m = individual_measurements[meter_index];
	paillier_plaintext_t *plain_m = paillier_plaintext_from_ui(m);

	enc_measurements[meter_index] = paillier_erkin_enc(NULL, public_key, plain_m, paillier_get_rand_devurandom, the_hash, ri);
	
}

/*
	Prints the results of the operations
*/
void print_results(){

	int real_total = 0;
	int i;
	for(i = 0; i < number_of_meters; i++){
		real_total += individual_measurements[i];
	}	

//	gmp_printf("Total regional consumption is M = %d (real is %Zd)\n", real_total, total_final);
//	printf("Meter's time: %f\nAG's time: %f\n", SM_time / number_of_meters, AG_time);

	printf("%f %f\n", SM_time / number_of_meters, AG_time);

}

/*
	Steps 1 and 2: Generate keys and random numbers
*/
void steps_1_and_2(){

	// Generating and sharing random numbers (Step 2)
	clock_t SM_begin = clock();
	
	int i;
	for(i = 0; i < number_of_meters; i++){
		struct meter_info *info = malloc(sizeof(struct meter_info));
		info->id = i;

		meter_generate_random_numbers(info);
		
		free(info);
	}
	
	clock_t SM_end = clock();
	SM_time += (double)(SM_end - SM_begin) / CLOCKS_PER_SEC;

}

/*
	Steps 3 and 4: Generates ri's and a hash, numbers needed to encrypt measurements
*/
void steps_3_and_4(){

	// Generating ri for every meter

	clock_t SM_begin = clock();

	int i;
	for(i = 0; i < number_of_meters; i++){
		struct meter_info *info = malloc(sizeof(struct meter_info));
		info->id = i;
		
		meter_calculate_ri(info);
		free(info);
		
		meter_compute_hash();

	}
	
	clock_t SM_end = clock();

	SM_time += (double)(SM_end - SM_begin) / CLOCKS_PER_SEC;

}

/*
	Step 5: Each meter encrypt its own measurement
*/
void step_5(){

	// Encrypting every meter's measurement

	clock_t SM_begin = clock();

	int i;
	for(i = 0; i < number_of_meters; i++){
		struct meter_info *info = malloc(sizeof(struct meter_info));
		info->id = i;
		
		meter_encript(info, set_of_ri[i]);
		
		free(info);
	}

	clock_t SM_end = clock();

	SM_time += (double)(SM_end - SM_begin) / CLOCKS_PER_SEC;

}

/*
	Step 6: Now the meters have all the measurements and are able to calculate the total
*/
void step_6(){

	clock_t SM_begin = clock();

	paillier_ciphertext_t *total_encrypted = paillier_create_enc_zero();
	// Multiplying all the encrypted measurements
	int i;
	for(i = 0; i < number_of_meters; i++){
		paillier_mul(public_key, total_encrypted, total_encrypted, enc_measurements[i]);
	}

	// Decrypting the multiplied measurements to obtain the total
	paillier_plaintext_t *total_decrypted = paillier_dec(NULL, public_key, secret_key, total_encrypted);	

	mpz_set(total_final, (total_decrypted->m));

	clock_t SM_end = clock();

	SM_time += (double)(SM_end - SM_begin) / CLOCKS_PER_SEC;
}

int main(int argc, char *argv[]){

	if(argv[1] == NULL){
		puts("Parameter required");
		exit(0);
	}

	number_of_meters = atoi(argv[1]);

	init_variables();
	steps_1_and_2();
	steps_3_and_4();
 	step_5();
	step_6();

   	print_results();

}

