#include <iostream>
#include <vector>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <map>

using namespace std;

// Определение типов для удобства
typedef uint32_t word32;
typedef uint32_t word64;
typedef vector<uint8_t> bytearray;

// Константы алгоритма
const size_t BLOCK_SIZE = 8; // Размер блока в байтах (64 бита)
const size_t INITIAL_KEY_SIZE = 7; // Исходный размер ключа (56 бит)
const size_t EXPANDED_KEY_SIZE = 32; // Размер расширенного ключа (256 бит)
const size_t WARNING_SIZE = 10240; // Порог предупреждения (10 КБ)
const size_t MAX_DATA_SIZE = 20480; // Максимальный размер данных (20 КБ)

// Таблица замен (S-блоки)
const uint8_t SBOX[8][16] = {
    // 8 S-блоков по 16 значений каждый
    {12, 4, 6, 2, 10, 5, 11, 9, 14, 8, 13, 7, 0, 3, 15, 1},
    {6, 8, 2, 3, 9, 10, 5, 12, 1, 14, 4, 7, 11, 13, 0, 15},
    {11, 3, 5, 8, 2, 15, 10, 13, 14, 1, 7, 4, 12, 9, 6, 0},
    {12, 8, 2, 1, 13, 4, 15, 6, 7, 0, 10, 5, 3, 14, 9, 11},
    {7, 15, 5, 10, 8, 1, 6, 13, 0, 9, 3, 14, 11, 4, 2, 12},
    {5, 13, 15, 6, 9, 2, 12, 10, 11, 7, 8, 1, 4, 3, 14, 0},
    {8, 14, 2, 5, 6, 9, 1, 12, 15, 4, 11, 0, 13, 10, 3, 7},
    {1, 7, 14, 13, 0, 5, 8, 3, 4, 15, 10, 6, 9, 12, 11, 2}
};

// Класс генератора псевдослучайных чисел
class PSG {
private:
    // Регистры сдвига для генерации случайных битов
    uint32_t rlz_1 = 0x1010101001100011101;
    uint32_t rlz_2[4] = {0x003b2c1d, 0x7e8f9a0b, 0x5c6d7e8f, 0x1a2b3c4a};
    
public:
    // Генерация ключа заданной длины
    bytearray generateKey(size_t length) {
        bytearray key;
        key.reserve(length);
        
        for(size_t i = 0; i < length * 8; i++) {
            // Генерация бита из первого регистра (LFSR)
            uint32_t bit1 = (rlz_1 & 0x01) ^ ((rlz_1 >> 3) & 0x01);
            rlz_1 = (rlz_1 >> 1) | (bit1 << 19);
            
            // Генерация бита из второго регистра (LFSR)
            uint32_t bit2 = (rlz_2[0] >> 7) ^ (rlz_2[0] >> 2);
            for(int j = 0; j < 3; j++) {
                uint32_t carry = rlz_2[j+1] & 0x01;
                rlz_2[j] = (rlz_2[j] >> 1) | (carry << 31);
            }
            rlz_2[3] = (rlz_2[3] >> 1) | (bit2 << 22);
            uint32_t bit_psp = (rlz_1 ^ rlz_2[0]) & 0x01;
            
            // Собираем байты из битов
            if(i % 8 == 0) {
                key.push_back(0);
            }
            key.back() = (key.back() << 1) | bit_psp;
        }
        
        return key;
    }
};

// Класс реализации алгоритма Магма
class magma {
private:
    word32 roundKeys[32]; // Ключи раундов
    static map<string, size_t> keyUsage; // Статистика использования ключей
    
    // Расширение ключа до нужного размера
    bytearray expandKey(const bytearray& key) {
        if (key.size() != INITIAL_KEY_SIZE) {
            throw runtime_error("Ключ должен быть 56 бит (7 байт)");
        }
        
        bytearray expanded(EXPANDED_KEY_SIZE);
        for (size_t i = 0; i < EXPANDED_KEY_SIZE; i++) {
            expanded[i] = key[i % INITIAL_KEY_SIZE];
        }
        
        copy(key.begin(), key.begin() + 4, expanded.end() - 4);
        return expanded;
    }
    
    // Функция T (нелинейное преобразование)
    word32 t(word32 a) {
        word32 result = 0;
        for (int i = 0; i < 8; i++) {
            uint8_t nibble = (a >> (4 * i)) & 0xF;
            result |= (word32)SBOX[i][nibble] << (4 * i);
        }
        return result;
    }
    
    // Функция G (основное преобразование)
    word32 g(word32 a, word32 k) {
        word32 sum = a + k;
        word32 substituted = t(sum);
        return (substituted << 11) | (substituted >> (32 - 11));
    }
    
    // Преобразование G в раунде
    void G(word32& a1, word32& a0, word32 k) {
        word32 temp = a0;
        a0 = g(a0, k) ^ a1;
        a1 = temp;
    }
    
    // Финальное преобразование G*
    word64 G_star(word32 a1, word32 a0, word32 k) {
        word32 g_result = g(a0, k);
        return ((word64)(g_result ^ a1) << 32) | a0;
    }
    
    // Генерация раундовых ключей
    void keyExpansion(const bytearray& key) {
        bytearray expandedKey = expandKey(key);
        
        word32 k[8];
        for (int i = 0; i < 8; i++) {
            k[i] = (expandedKey[4*i] << 24) | (expandedKey[4*i+1] << 16) 
                 | (expandedKey[4*i+2] << 8) | expandedKey[4*i+3];
        }
        
        // Первые 24 раунда
        for (int i = 0; i < 24; i++) {
            roundKeys[i] = k[i % 8];
        }
        // Последние 8 раундов
        for (int i = 24; i < 32; i++) {
            roundKeys[i] = k[7 - (i % 8)];
        }
    }
    
    // Добавление padding к данным
    bytearray pad(const bytearray& data) {
        size_t padLen = BLOCK_SIZE - (data.size() % BLOCK_SIZE);
        bytearray padded = data;
        padded.push_back(0x80); // Стартовый маркер padding
        while (padded.size() % BLOCK_SIZE != 0) {
            padded.push_back(0x00); // Заполнение нулями
        }
        return padded;
    }
    
    // Удаление padding из данных
    bytearray unpad(const bytearray& data) {
        if (data.empty()) return data;
        
        auto it = find(data.rbegin(), data.rend(), 0x80);
        if (it != data.rend()) {
            return bytearray(data.begin(), data.end() - (it - data.rbegin() + 1));
        }
        return data;
    }
    
public:
    // Конструктор с инициализацией ключей
    magma(const bytearray& key) {
        keyExpansion(key);
    }
    
    // Шифрование одного блока
    word64 encryptBlock(word64 block) {
        word32 a1 = block >> 32;
        word32 a0 = block & 0xFFFFFFFF;
        
        // 31 раунд прямого преобразования
        for (int i = 0; i < 31; i++) {
            G(a1, a0, roundKeys[i]);
        }
        // Финальный раунд
        return G_star(a1, a0, roundKeys[31]);
    }
    
    // Дешифрование одного блока
    word64 decryptBlock(word64 block) {
        word32 a1 = block >> 32;
        word32 a0 = block & 0xFFFFFFFF;
        
        // 31 раунд обратного преобразования
        for (int i = 31; i > 0; i--) {
            G(a1, a0, roundKeys[i]);
        }
        // Финальный раунд
        return G_star(a1, a0, roundKeys[0]);
    }
    
    // Шифрование данных с учетом лимитов
    bytearray encrypt(const bytearray& plaintext, const string& keyFile) {
        bytearray padded = pad(plaintext);
        bytearray ciphertext;
        
        // Обновление статистики использования ключа
        keyUsage[keyFile] += padded.size();
        if (keyUsage[keyFile] > MAX_DATA_SIZE) {
            throw runtime_error("Превышен лимит данных (20КБ) для этого ключа");
        } else if (keyUsage[keyFile] > WARNING_SIZE) {
            cerr << "Предупреждение: Зашифровано " << keyUsage[keyFile] 
                 << " байт с этим ключом. Рекомендуется сменить ключ." << endl;
        }
        
        // Зашифрование по блокам
        for (size_t i = 0; i < padded.size(); i += BLOCK_SIZE) {
            word64 block = 0;
            for (size_t j = 0; j < BLOCK_SIZE; j++) {
                block = (block << 8) | padded[i + j];
            }
            
            word64 encrypted = encryptBlock(block);
            
            // Разбиение зашифрованного блока на байты
            for (int j = BLOCK_SIZE - 1; j >= 0; j--) {
                ciphertext.push_back((encrypted >> (8 * j)) & 0xFF);
            }
        }
        return ciphertext;
    }
    
    // Расшифрование данных
    bytearray decrypt(const bytearray& ciphertext) {
        if (ciphertext.size() % BLOCK_SIZE != 0) {
            throw runtime_error("Размер шифртекста должен быть кратен размеру блока");
        }
        
        bytearray plaintext;
        for (size_t i = 0; i < ciphertext.size(); i += BLOCK_SIZE) {
            word64 block = 0;
            for (size_t j = 0; j < BLOCK_SIZE; j++) {
                block = (block << 8) | ciphertext[i + j];
            }
            
            word64 decrypted = decryptBlock(block);
            
            // Разбиение расшифрованного блока на байты
            for (int j = BLOCK_SIZE - 1; j >= 0; j--) {
                plaintext.push_back((decrypted >> (8 * j)) & 0xFF);
            }
        }
        return unpad(plaintext);
    }
    
    // Утилиты для модификации данных
    
    // Удаление байта по позиции
    static bytearray removeByte(const bytearray& data, size_t pos) {
        if (pos >= data.size()) return data;
        bytearray modified = data;
        modified.erase(modified.begin() + pos);
        return modified;
    }
    
    // Удаление блока по номеру
    static bytearray removeBlock(const bytearray& data, size_t blockNum) {
        size_t pos = blockNum * BLOCK_SIZE;
        if (pos >= data.size()) return data;
        
        bytearray modified = data;
        size_t toRemove = min(BLOCK_SIZE, data.size() - pos);
        modified.erase(modified.begin() + pos, modified.begin() + pos + toRemove);
        return modified;
    }
    
    // Добавление блока
    static bytearray addBlock(const bytearray& data, const bytearray& block) {
        bytearray modified = data;
        modified.insert(modified.end(), block.begin(), block.end());
        return modified;
    }
    
    // Перестановка блоков местами
    static bytearray swapBlocks(const bytearray& data, size_t block1, size_t block2) {
        size_t pos1 = block1 * BLOCK_SIZE;
        size_t pos2 = block2 * BLOCK_SIZE;
        if (pos1 + BLOCK_SIZE > data.size() || pos2 + BLOCK_SIZE > data.size()) {
            return data;
        }
        
        bytearray modified = data;
        for (size_t i = 0; i < BLOCK_SIZE; i++) {
            swap(modified[pos1 + i], modified[pos2 + i]);
        }
        return modified;
    }
};

// Инициализация статического члена класса
map<string, size_t> magma::keyUsage;

// Функции работы с файлами

// Чтение файла в массив байтов
bytearray readFile(const string& filename) {
    ifstream file(filename, ios::binary | ios::ate);
    if (!file) throw runtime_error("Не удалось открыть файл: " + filename);
    
    streamsize size = file.tellg();
    file.seekg(0, ios::beg);
    
    bytearray buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        throw runtime_error("Ошибка чтения файла: " + filename);
    }
    
    return buffer;
}

// Запись массива байтов в файл
void writeFile(const string& filename, const bytearray& data) {
    ofstream file(filename, ios::binary);
    if (!file) throw runtime_error("Не удалось открыть файл для записи: " + filename);
    
    if (!file.write(reinterpret_cast<const char*>(data.data()), data.size())) {
        throw runtime_error("Ошибка записи в файл: " + filename);
    }
}

// Генерация случайного ключа
bytearray generateRandomKey() {
    PSG prng;
    return prng.generateKey(INITIAL_KEY_SIZE);
}

// Преобразование байтов в hex-строку
string bytesToHex(const bytearray& data) {
    stringstream ss;
    ss << hex << setfill('0');
    for (uint8_t byte : data) ss << setw(2) << static_cast<int>(byte);
    return ss.str();
}

// Тестирование устойчивости к повреждениям
void testCorruption(const string& cipherFile, const bytearray& key) {
    magma cipher(key);
    bytearray ciphertext = readFile(cipherFile);
    
    // Тест 1: удаление одного байта
    bytearray modified1 = magma::removeByte(ciphertext, 10);
    writeFile("corrupted1.enc", modified1);
    try {
        bytearray decrypted1 = cipher.decrypt(modified1);
        writeFile("decrypted1.txt", decrypted1);
        cout << "Расшифровка после удаления 1 байта: " 
             << (decrypted1.size() ? "частично успешна" : "не удалась") << endl;
    } catch (...) {
        cout << "Расшифровка не удалась после удаления 1 байта" << endl;
    }
    
    // Тест 2: удаление целого блока
    bytearray modified2 = magma::removeBlock(ciphertext, 1);
    writeFile("corrupted2.enc", modified2);
    try {
        bytearray decrypted2 = cipher.decrypt(modified2);
        writeFile("decrypted2.txt", decrypted2);
        cout << "Расшифровка после удаления блока: " 
             << (decrypted2.size() ? "частично успешна" : "не удалась") << endl;
    } catch (...) {
        cout << "Расшифровка не удалась после удаления блока" << endl;
    }
    
    // Тест 3: добавление случайного блока
    bytearray randomBlock(BLOCK_SIZE, 0xAA);
    bytearray modified3 = magma::addBlock(ciphertext, randomBlock);
    writeFile("corrupted3.enc", modified3);
    try {
        bytearray decrypted3 = cipher.decrypt(modified3);
        writeFile("decrypted3.txt", decrypted3);
        cout << "Расшифровка после добавления блока: " 
             << (decrypted3.size() ? "частично успешна" : "не удалась") << endl;
    } catch (...) {
        cout << "Расшифровка не удалась после добавления блока" << endl;
    }
    
    // Тест 4: перестановка блоков местами
    if (ciphertext.size() >= 2 * BLOCK_SIZE) {
        bytearray modified4 = magma::swapBlocks(ciphertext, 0, 1);
        writeFile("corrupted4.enc", modified4);
        try {
            bytearray decrypted4 = cipher.decrypt(modified4);
            writeFile("decrypted4.txt", decrypted4);
            cout << "Расшифровка после перестановки блоков: " 
                 << (decrypted4.size() ? "частично успешна" : "не удалась") << endl;
        } catch (...) {
            cout << "Расшифровка не удалась после перестановки блоков" << endl;
        }
    }
}
int main() {
    try {
        // Генерация ключа
        cout << "Генерация 56-битного ключа..." << endl;
        bytearray key = generateRandomKey();
        cout << "Сгенерированный ключ: " << bytesToHex(key) << endl;
        writeFile("key.key", key);
        
        // Шифрование файла
        magma cipher(key);
        bytearray plaintext = readFile("input2.txt");
        bytearray ciphertext = cipher.encrypt(plaintext, "key.key");
        writeFile("output.enc", ciphertext);
        cout << "Файл успешно зашифрован и сохранен как output.enc" << endl;
        
        // Дешифрование файла
        bytearray decrypted = cipher.decrypt(ciphertext);
        writeFile("decrypted.txt", decrypted);
        cout << "Файл успешно расшифрован и сохранен как decrypted.txt" << endl;
        
        // Проверка целостности
        if (plaintext == decrypted) {
            cout << "Проверка: исходный и расшифрованный файлы идентичны!" << endl;
        } else {
            cout << "Внимание: исходный и расшифрованный файлы различаются!" << endl;
        }
        
        // Тестирование устойчивости к повреждениям
        cout << "\nТестирование эффектов искажения данных:" << endl;
        testCorruption("output.enc", key);
        
        // Проверка ограничения на объем данных
        try {
            bytearray largePlaintext(MAX_DATA_SIZE + 1, 0xAA);
            cipher.encrypt(largePlaintext, "key.key");
            cout << "Ошибка: программа должна блокировать шифрование более 20КБ" << endl;
        } catch (const exception& e) {
            cout << "Проверка лимита: " << e.what() << endl;
        }
        
    } catch (const exception& e) {
        cerr << "Ошибка: " << e.what() << endl;
        return 1;
    }
    
    return 0;
}
